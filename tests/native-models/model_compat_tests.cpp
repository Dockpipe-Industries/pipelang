// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include <fstream>
#include <iostream>

namespace {
using namespace pipelang::tools;
struct Case {
    std::string name;
    std::string source;
    std::string message;
};
class Oracle {
public:
    Oracle(fs::path compiler, fs::path output, std::string suite)
        : compiler_(std::move(compiler)), output_(std::move(output)), suite_(std::move(suite)) {
        require(compiler_.is_absolute() && output_.is_absolute(), "absolute compiler and proof paths required");
        require(!fs::exists(output_), "oracle output must be fresh");
        fs::create_directories(output_);
        compilerHash_ = fileHash(compiler_);
    }
    Json invoke(const std::string& source, const std::string& selectedNamespace = {}) const {
        const Json request{{"protocol", "pipelang.native-model.v1"}, {"source", source},
            {"namespace", selectedNamespace.empty() ? (suite_ == "protocol" ? "test_models" : "pipelang_models") : selectedNamespace}};
        const auto result = process({compiler_.string()}, canonical(request), std::chrono::seconds(10), 4U << 20);
        require(result.exit == 0 && result.diagnostic.empty(), "native model process refused transport: " + result.diagnostic);
        const auto response = parseJson(result.output, 4U << 20);
        require(response.at("protocol") == "pipelang.native-model.v1", "native model protocol changed");
        return response;
    }
    Json admitted(const std::string& name, const std::string& source, const std::string& selectedNamespace = {}) {
        const auto result = invoke(source, selectedNamespace);
        exactObject(result, {"protocol", "ok", "header", "manifest"}, {}, "admitted response");
        require(result.at("ok") == true && result.at("manifest").at("protocol") == "pipelang.native-model-manifest.v1", "expected admission: " + name);
        record(name, source, "admitted");
        return result;
    }
    void refused(const Case& item) {
        const auto result = invoke(item.source);
        require(result.at("ok") == false && !result.contains("header") && !result.contains("manifest"), "expected refusal: " + item.name);
        require(result.at("diagnostics").is_array() && !result.at("diagnostics").empty(), "refusal has no diagnostics: " + item.name);
        for (const auto& diagnostic : result.at("diagnostics")) {
            require(diagnostic.at("offset").is_number_integer() && diagnostic.at("offset").get<std::int64_t>() >= 0,
                "invalid diagnostic offset: " + item.name);
            require(!text(diagnostic.at("message"), "diagnostic").empty(), "empty refusal diagnostic");
        }
        require(result.at("diagnostics").front().at("message").get<std::string>().find(item.message) != std::string::npos,
            "refusal diagnostic changed: " + item.name + ": " + result.dump());
        record(item.name, item.source, "refused");
    }
    void malformed(const std::string& name, const std::string& bytes) {
        const auto result = process({compiler_.string()}, bytes, std::chrono::seconds(10), 4U << 20);
        require(result.exit == 2 && result.output.empty(), "malformed transport was admitted: " + name);
        record(name, bytes, "transport-refused");
    }
    void generated(const std::string& name, const Json& response) const {
        atomicWrite(output_ / (name + ".hpp"), response.at("header").get<std::string>());
        atomicWrite(output_ / (name + ".manifest.json"), canonical(response.at("manifest")));
    }
    void finish() const {
        require(fileHash(compiler_) == compilerHash_, "selected native compiler changed during oracle");
        atomicWrite(output_ / "acceptance.json", canonical({{"status", "passed"}, {"suite", suite_},
            {"compiler_sha256", compilerHash_}, {"cases", cases_}}));
        std::cout << suite_ << ": " << cases_.size() << " native C++ cases passed\n";
    }
private:
    void record(const std::string& name, const std::string& source, const std::string& expected) {
        require(names_.insert(name).second, "duplicate test identity");
        cases_.push_back({{"id", suite_ + "/" + name}, {"input_sha256", sha256(source)}, {"outcome", expected}});
        atomicWrite(output_ / "progress.json", canonical(cases_));
    }
    fs::path compiler_;
    fs::path output_;
    std::string suite_;
    std::string compilerHash_;
    std::set<std::string> names_;
    Json cases_ = Json::array();
};
void containment() {
    std::ifstream membership("/proc/self/cgroup");
    std::string line;
    fs::path group;
    while (std::getline(membership, line)) {
        if (line.starts_with("0::/"))
            group = fs::path("/sys/fs/cgroup") / line.substr(4);
    }
    require(!group.empty(), "native oracle requires cgroup v2 unit containment");
    for (const auto& [name, expected] : std::map<std::string, std::string>{
            {"memory.max", "1073741824\n"}, {"memory.high", "734003200\n"},
            {"memory.swap.max", "0\n"}, {"pids.max", "128\n"}})
        require(readFile(group / name, 128) == expected, "native oracle containment differs: " + name);
}
void protocol(Oracle& oracle, const fs::path& root) {
    const auto response = oracle.admitted("models-fixture", readFile(root / "tests/native-models/Models.pipe", 65536));
    const auto& model = response.at("manifest").at("classes").front();
    require(model.at("name") == "Model", "model name differs");
    for (const auto& field : model.at("fields"))
        require(field.at("name") != "Service" && field.at("name") != "secret", "private capability leaked in manifest");
    require(model.at("constructor").at("parameters") == Json::array({{{"name", "service"}, {"type", "Processor"}},
        {{"name", "initial"}, {"type", "Payload"}}}), "constructor parameter manifest differs");
    oracle.generated("Models", oracle.admitted("runtime-fixture", readFile(root / "tests/native-models/Models.pipe", 65536), "pipelang_models"));
    oracle.admitted("empty-model", "public Class Empty { public int count = 0; }");
    const std::string service = "public Interface Store { Task<Result<void,string>> Save(CancellationToken token); } ";
    const std::vector<Case> cases{
        {"pure-mutation", "public Class Model { public int count; public pure int Read() { count = 2; return count; } }", "pure methods cannot mutate"},
        {"pure-action-call", "public Class Model { public void Edit() {} public pure int Read() { int value = Edit(); return value; } }", "pure methods cannot call mutating helpers"},
        {"recursive-query", "public Class Model { public pure int Read() => Read(); }", "recursive local call"},
        {"recursive-record", "public Record Broken { public Broken value; }", "recursive record"},
        {"member-collision", "public Class Model { public int value; public pure int value() => 1; }", "duplicate method"},
        {"local-type", "public Class Model { public void Edit(string value) { int number = value; } }", "expected int"},
        {"integer-overflow", "public Class Model { public pure int Read() => 9223372036854775808; }", "outside signed64"},
        {"record-member", "public Record Value { public int number; } public Class Model { public pure Value Read() => new Value { absent = 1 }; }", "unknown or duplicate record"},
        {"unsupported-generic", "public Class Model { public Unsupported<int> value; }", "unsupported generic"},
        {"recursive-task", "public Class Model { public async Task<Result<void,string>> Run() { Result<void,string> result = await Run(); return result; } }", "recursive local call"},
        {"missing-return", "public Class Model { public pure int Read(bool flag) { if (flag) return 1; } }", "every path"},
        {"conditional-condition", "public Class Model { public pure int Read() => 1 ? 2 : 3; }", "expected bool"},
        {"conditional-arm", "public Class Model { public pure int Read(bool flag) => flag ? 1 : false; }", "expected int"},
        {"conditional-record", "public Record Value { public int number; } public Class Model { public pure Value Read(bool flag) => flag ? Value(1) : 2; }", "expected Value"},
        {"conditional-result", "public Class Model { public pure Result<int,string> Read(bool flag) => flag ? Result<int,string>.Success(1) : Result<bool,string>.Success(true); }", "expected Result<int,string>"},
        {"recursive-action", "public Class Model { public void Recurse() { Recurse(); } }", "recursive local call"},
        {"result-capability-field", service + "public Class Model { private Store service; public Result<Store,string> leak; }", "Result payloads must recursively contain owned values"},
        {"result-capability-query", service + "public Class Model { private Store service; public pure Result<Store,string> Leak() => Result<Store,string>.Success(service); }", "Result payloads must recursively contain owned values"},
        {"nested-result-capability", service + "public Record Nested { public Result<Result<Store,string>,string> leak; }", "Result payloads must recursively contain owned values"},
        {"unawaited-capability", service + "public Class Model { private Store service; public pure bool Read(CancellationToken token) => service.Save(token).IsSuccess; }", "host calls must be directly awaited"}};
    require(cases.size() == 20, "protocol refusal inventory differs");
    for (const auto& item : cases)
        oracle.refused(item);
    std::string depth = "public Class Model { public pure int F0() => 1; ";
    for (int index = 1; index < 34; ++index)
        depth += "public pure int F" + std::to_string(index) + "() => F" + std::to_string(index - 1) + "(); ";
    oracle.refused({"helper-depth", depth + "}", "call depth limit"});
    std::string repeated = "public Class Model { public pure int F0() => 1; ";
    std::string actions = "public Class Model { public int count; private void F0() { count = count + 1; } ";
    for (int index = 1; index < 18; ++index) {
        const auto current = std::to_string(index);
        const auto previous = std::to_string(index - 1);
        repeated += "public pure int F" + current + "() => F" + previous + "() + F" + previous + "(); ";
        actions += "private void F" + current + "() { F" + previous + "(); F" + previous + "(); } ";
    }
    oracle.refused({"query-work", repeated + "}", "evaluation work limit"});
    oracle.refused({"action-work", actions + "}", "evaluation work limit"});
    std::string layouts = "public Record R0 { public int value; } ";
    for (int index = 1; index < 10; ++index)
        layouts += "public Record R" + std::to_string(index) + " { public R" + std::to_string(index - 1) + " left; public R" + std::to_string(index - 1) + " right; } ";
    oracle.refused({"expanded-layout", layouts, "expanded value layout limit"});
    oracle.refused({"source-bytes", std::string(65537, ' '), "source byte limit"});
    oracle.malformed("duplicate-key", R"({"protocol":"pipelang.native-model.v1","source":"","source":""})");
    oracle.malformed("wrong-protocol", R"({"protocol":"wrong","source":""})");
    oracle.malformed("unknown-field", R"({"protocol":"pipelang.native-model.v1","source":"","surprise":true})");
    oracle.malformed("wrong-source-type", R"({"protocol":"pipelang.native-model.v1","source":42})");
}
void fundamentals(Oracle& oracle, const fs::path& root) {
    const auto result = oracle.admitted("fixture", readFile(root / "tests/native-models/Fundamentals.pipe", 65536));
    bool stored = false;
    bool label = false;
    for (const auto& model : result.at("manifest").at("classes")) {
        if (model.at("name") != "Fundamentals")
            continue;
        for (const auto& field : model.at("fields")) {
            stored |= field.at("name") == "stored" && field.at("type") == "float";
            label |= field.at("name") == "label" && field.at("type") == "Optional<Label>";
        }
    }
    require(stored && label, "fundamentals field manifest differs");
    const std::string service = "public Interface Store { Task<Result<void,string>> Save(CancellationToken token); } ";
    const std::vector<Case> cases{
        {"float-from-int", "public Class M { public float value = 1; }", ""},
        {"int-from-float", "public Class M { public int value = 1.0; }", ""},
        {"mixed-add", "public Class M { public pure float Read(float value) => value + 1; }", ""},
        {"mixed-compare", "public Class M { public pure bool Read(float value) => value < 1; }", ""},
        {"mixed-conditional", "public Class M { public pure float Read(bool flag) => flag ? 1.0 : 1; }", ""},
        {"incomplete-exponent", "public Class M { public pure float Read() => 1e+; }", ""},
        {"finite-int", "public Class M { public pure bool Read() => float.IsFinite(1); }", ""},
        {"finite-bool", "public Class M { public pure bool Read() => float.IsFinite(true); }", ""},
        {"finite-arity", "public Class M { public pure bool Read() => float.IsFinite(); }", ""},
        {"float-member", "public Class M { public pure float Read() => float.Unknown(1.0); }", ""},
        {"optional-payload", "public Class M { public Optional<float> value = Optional<float>.Some(1); }", ""},
        {"optional-type", "public Class M { public Optional<float> value = Optional<int>.None(); }", ""},
        {"optional-void", "public Class M { public Optional<void> value; }", ""},
        {"optional-token", "public Class M { public Optional<CancellationToken> value; }", ""},
        {"optional-arity", "public Class M { public Optional<int,string> value; }", ""},
        {"some-arity", "public Class M { public pure int Read() => Optional<int>.Some().Value; }", ""},
        {"none-arity", "public Class M { public pure int Read() => Optional<int>.None(1).Value; }", ""},
        {"block-scope", "public Class M { public pure int Read() { { int local = 1; } return local; } }", ""},
        {"local-shadow", "public Class M { public pure int Read() { int local = 1; { int local = 2; } return local; } }", ""},
        {"parameter-shadow", "public Class M { public pure int Read(int local) { { int local = 2; } return local; } }", ""},
        {"partial-return", "public Class M { public pure int Read(bool flag) { { if (flag) return 1; } } }", ""},
        {"optional-recursion", "public Record Recursive { public Optional<Recursive> value; }", ""},
        {"result-recursion", "public Record Recursive { public Result<Optional<Recursive>,string> value; }", ""},
        {"optional-capability", service + "public Record R { public Optional<Store> value; }", ""},
        {"optional-result-capability", service + "public Record R { public Optional<Result<Store,string>> value; }", ""},
        {"result-optional-capability", service + "public Record R { public Result<Optional<Store>,string> value; }", ""},
        {"nested-token", "public Record R { public Optional<Result<Optional<CancellationToken>,string>> value; }", ""},
        {"decimal-overflow", "public Class M { public float value = " + std::string(400, '9') + ".0; }", ""}};
    require(cases.size() == 28, "fundamentals refusal inventory differs");
    for (const auto& item : cases)
        oracle.refused(item);
    const auto defaults = oracle.admitted("numeric-defaults", "public Record NumericDefaults { public bool finite = float.IsFinite(0.0); } public Class DefaultOwner { public NumericDefaults value; }");
    require(defaults.at("header").get<std::string>().find("#include <pipelang/model/numeric.hpp>") != std::string::npos, "numeric default omitted numeric runtime");
    oracle.generated("NumericDefaults", defaults);
    oracle.refused({"exponent-overflow", "public Class M { public float value = 1e309; }", ""});
    oracle.refused({"exponent-underflow", "public Class M { public float value = 1e-999; }", ""});
    std::string deep = "public Class M { public void Go() { ";
    for (int index = 0; index < 5000; ++index)
        deep += "if (true) ";
    oracle.refused({"branch-depth", deep + "return; } }", ""});
    oracle.generated("Fundamentals", result);
}
void asyncComposition(Oracle& oracle, const fs::path& root) {
    const auto result = oracle.admitted("fixture", readFile(root / "tests/native-models/async-composition.pipe", 65536));
    const std::string child = "private async Task<Result<int,string>> Child() { return Result<int,string>.Success(1); }";
    const std::vector<Case> cases{
        {"unawaited-child", "public Class M { " + child + " public void Go() { Child(); } }", ""},
        {"await-in-pure", "public Class M { " + child + " public pure int Go() { Result<int,string> r = await Child(); return r.Value; } }", ""},
        {"await-in-sync", "public Class M { " + child + " public int Go() { Result<int,string> r = await Child(); return r.Value; } }", ""},
        {"return-await", "public Class M { " + child + " public async Task<Result<int,string>> Go() { return await Child(); } }", ""},
        {"nested-await", "public Class M { " + child + " public async Task<Result<int,string>> Go() { int n = 1 + (await Child()).Value; return Result<int,string>.Success(n); } }", ""},
        {"task-field", "public Class M { " + child + " public Task<Result<int,string>> leaked; }", ""},
        {"task-local", "public Class M { " + child + " public async Task<Result<int,string>> Go() { Task<Result<int,string>> leaked = Child(); return Result<int,string>.Success(1); } }", ""},
        {"recursive-await", "public Class M { public async Task<Result<int,string>> Go() { Result<int,string> r = await Go(); return r; } }", ""},
        {"mutual-await", "public Class M { public async Task<Result<int,string>> A() { Result<int,string> r = await B(); return r; } private async Task<Result<int,string>> B() { Result<int,string> r = await A(); return r; } }", ""}};
    require(cases.size() == 9, "async composition refusal inventory differs");
    for (const auto& item : cases)
        oracle.refused(item);
    std::string depth = "public Class M { private async Task<Result<int,string>> F0() { return Result<int,string>.Success(1); } ";
    for (int index = 1; index < 34; ++index)
        depth += "private async Task<Result<int,string>> F" + std::to_string(index) + "() { Result<int,string> r = await F" + std::to_string(index - 1) + "(); return r; } ";
    oracle.refused({"await-depth", depth + "}", ""});
    oracle.generated("AsyncComposition", result);
}
void cleanup(Oracle& oracle, const fs::path& root) {
    const auto result = oracle.admitted("fixture", readFile(root / "tests/native-models/FailureCleanup.pipe", 65536));
    const std::string child = "private async Task<Result<int,string>> Child() { return Result<int,string>.Success(1); }";
    const std::vector<Case> cases{
        {"rethrow-outside", "public Class M { public void Run() { throw; } }", ""},
        {"throw-int", "public Class M { public void Run() { throw 1; } }", ""},
        {"throw-string", "public Class M { public void Run() { throw \"text\"; } }", ""},
        {"bare-try", "public Class M { public void Run() { try {} } }", ""},
        {"catch-string", "public Class M { public void Run() { try {} catch (string error) {} } }", ""},
        {"catch-no-name", "public Class M { public void Run() { try {} catch (Error) {} } }", ""},
        {"catch-scope", "public Class M { public void Run() { try {} catch (Error error) {} string text = error.Message; } }", ""},
        {"catch-shadow", "public Class M { public void Run() { int error = 0; try {} catch (Error error) {} } }", ""},
        {"error-payload", "public Class M { public void Run() { Error error = Error(1); } }", ""},
        {"error-mutation", "public Class M { public void Run() { Error error = Error(\"owned\"); error.Message = \"mutated\"; } }", ""},
        {"finally-return", "public Class M { public void Run() { try {} finally { return; } } }", ""},
        {"finally-break", "public Class M { public void Run() { while (true) { try {} finally { break; } } } }", ""},
        {"finally-continue", "public Class M { public void Run() { while (true) { try {} finally { continue; } } } }", ""},
        {"try-scope", "public Class M { public void Run() { try { int scoped = 1; } finally { int value = scoped; } } }", ""},
        {"catch-finally-scope", "public Class M { public void Run() { try {} catch (Error error) {} finally { string text = error.Message; } } }", ""},
        {"finally-rethrow", "public Class M { public void Run() { try {} finally { throw; } } }", ""},
        {"escaped-rethrow", "public Class M { public void Run() { try { throw Error(\"x\"); } catch (Error error) {} throw; } }", ""},
        {"cleanup-cycle", "public Class M { private void Cycle() { try {} finally { Cycle(); } } public void Run() { Cycle(); } }", ""},
        {"finally-await", "public Class M { " + child + " public async Task<Result<int,string>> Run() { try {} finally { Result<int,string> result = await Child(); } return Result<int,string>.Success(1); } }", ""},
        {"nested-finally-await", "public Class M { " + child + " public async Task<Result<int,string>> Run() { try {} finally { if (true) { Result<int,string> result = await Child(); } } return Result<int,string>.Success(1); } }", ""},
        {"sync-catch-await", "public Class M { " + child + " public int Run() { try { throw Error(\"x\"); } catch (Error error) { Result<int,string> result = await Child(); return result.Value; } } }", ""}};
    require(cases.size() == 21, "cleanup refusal inventory differs");
    for (const auto& item : cases)
        oracle.refused(item);
    oracle.generated("FailureCleanup", result);
}
void iteration(Oracle& oracle, const fs::path& root) {
    const auto result = oracle.admitted("fixture", readFile(root / "tests/native-models/iteration-tests.pipe", 65536));
    const std::vector<Case> cases{
        {"break-outside", "public Class M { public void Go() { break; } }", ""},
        {"continue-outside", "public Class M { public void Go() { continue; } }", ""},
        {"while-condition", "public Class M { public void Go() { while (1) {} } }", ""},
        {"foreach-scalar", "public Class M { public void Go() { foreach (int item in 1) {} } }", ""},
        {"foreach-element", "public Class M { public void Go() { List<int> values = List<int>.Empty(); foreach (string item in values) {} } }", ""},
        {"foreach-scope", "public Class M { public void Go() { List<int> values = List<int>.Empty(); foreach (int item in values) {} int copy = item; } }", ""},
        {"foreach-shadow", "public Class M { public void Go() { List<int> values = List<int>.Empty(); int item = 0; foreach (int item in values) {} } }", ""},
        {"list-void", "public Class M { public List<void> values; }", ""},
        {"list-token", "public Class M { public List<CancellationToken> values; }", ""},
        {"list-add-type", "public Class M { public void Go() { List<int> values = List<int>.Empty(); values.Add(1.0); } }", ""},
        {"list-index-type", "public Class M { public pure int Go() { List<int> values = List<int>.Empty(); return values.At(0.0); } }", ""},
        {"pure-list-mutation", "public Class M { public List<int> values; public pure int Go() { values.Add(1); return 0; } }", ""}};
    require(cases.size() == 12, "iteration refusal inventory differs");
    for (const auto& item : cases)
        oracle.refused(item);
    oracle.generated("Iteration", result);
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        Arguments arguments(argc, argv);
        const auto suite = arguments.take("--suite");
        const fs::path compiler = arguments.take("--compiler");
        const fs::path root = arguments.take("--source-root");
        const fs::path output = arguments.take("--output");
        arguments.finish();
        containment();
        Oracle oracle(compiler, output, suite);
        if (suite == "protocol")
            protocol(oracle, root);
        else if (suite == "fundamentals")
            fundamentals(oracle, root);
        else if (suite == "iteration")
            iteration(oracle, root);
        else if (suite == "async-composition")
            asyncComposition(oracle, root);
        else if (suite == "failure-cleanup")
            cleanup(oracle, root);
        else
            throw std::runtime_error("unknown native compatibility suite");
        oracle.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native compatibility oracle: " << error.what() << '\n';
        return 1;
    }
}
