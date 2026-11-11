// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include "NativeXmlViewTests.hpp"
#include <iostream>

namespace {
using namespace pipelang::tools;
class Oracle {
public:
    Oracle(fs::path root, fs::path sdk, fs::path output)
        : root_(std::move(root)), sdk_(std::move(sdk)), output_(std::move(output)) {
        require(!fs::exists(output_), "extended oracle requires fresh output");
        fs::create_directories(output_);
    }
    Json invoke(const Json& request, bool view = false) {
        const auto compiler = nativeExecutable(sdk_ / (view ? "bin/pipelang-model-viewc" : "bin/pipelang-modelc"));
        const auto result = process({compiler.string()}, canonical(request), std::chrono::seconds(15), 8U << 20);
        require(result.exit == 0 && result.diagnostic.empty(), "compiler transport failed: " + result.diagnostic);
        const auto response = parseJson(result.output, 8U << 20);
        require(response.at("protocol") == request.at("protocol"), "compiler response protocol differs");
        return response;
    }
    Json admitted(const std::string& name, const Json& request, bool view = false) {
        const auto response = invoke(request, view);
        require(response.at("ok") == true && response.contains("header") && response.contains("manifest"),
            "expected admission " + name + ": " + response.dump());
        require(response == invoke(request, view), "nondeterministic generation: " + name);
        atomicWrite(output_ / (name + ".hpp"), response.at("header").get<std::string>());
        atomicWrite(output_ / (name + ".manifest.json"), canonical(response.at("manifest")));
        if (response.contains("types_header"))
            atomicWrite(output_ / "Types.hpp", response.at("types_header").get<std::string>());
        if (response.contains("constants_header"))
            atomicWrite(output_ / (name + ".constants.hpp"), response.at("constants_header").get<std::string>());
        record(name, request, "admitted");
        return response;
    }
    void refused(const std::string& name, const Json& request, bool view = false, const std::string& expected = {}) {
        const auto response = invoke(request, view);
        require(response.at("ok") == false && !response.contains("header") && !response.contains("manifest") &&
            response.at("diagnostics").is_array() && !response.at("diagnostics").empty(), "expected refusal " + name + ": " + response.dump());
        if (!expected.empty())
            require(response.at("diagnostics").front().at("message").get<std::string>().find(expected) != std::string::npos,
                    "refusal diagnostic changed: " + name + ": " + response.dump());
        for (const auto& diagnostic : response.at("diagnostics")) {
            require(!text(diagnostic.at("message"), "diagnostic").empty(), "empty refusal diagnostic");
            const auto position = diagnostic.at(view ? "start" : "offset").get<std::int64_t>();
            require(position >= 0, "negative source diagnostic position");
        }
        record(name, request, "refused");
    }
    std::string source(const std::string& path) const { return readFile(root_ / path, 65536); }
    void finish() const {
        atomicWrite(output_ / "acceptance.json", canonical({{"status", "passed"}, {"cases", cases_}}));
        std::cout << "extended model/view oracle: " << cases_.size() << " cases passed\n";
    }
private:
    void record(const std::string& name, const Json& request, const std::string& result) {
        cases_.push_back({{"id", name}, {"input_sha256", sha256(canonical(request))}, {"outcome", result}});
    }
    fs::path root_, sdk_, output_;
    Json cases_ = Json::array();
};
Json sourceRequest(const std::string& source, const std::string& space, bool objects = false) {
    Json request{{"protocol", objects ? "pipelang.native-model.v3" : "pipelang.native-model.v1"},
        {"source", source}, {"namespace", space}};
    if (objects)
        request["models"] = {"App"};
    return request;
}
std::string replace(std::string source, const std::string& old, const std::string& replacement) {
    const auto position = source.find(old);
    require(position != std::string::npos, "test mutation did not match");
    source.replace(position, old.size(), replacement);
    return source;
}
void run(Oracle& oracle) {
    const std::string statelessWorker = R"pipe(
public Class Numeric {
    public Numeric() {}
    private pure int Add(int first, int second) => first + second;
    public pure int Twice(int value) => Add(value, value);
    public void Calculate(List<int> inputs) {
        ConcurrentDictionary<int, int> completed = ConcurrentDictionary<int, int>.Empty();
        parallel foreach (int input in inputs) {
            completed.Set(input, Twice(input));
        }
    }
}
public Class App {
    private Numeric numeric = new Numeric();
    public void Calculate(List<int> inputs) { numeric.Calculate(inputs); }
}
)pipe";
    const auto stateless = oracle.admitted("StatelessObjectWorker",
        sourceRequest(statelessWorker, "stateless_object_worker", true));
    const auto statelessHeader = stateless.at("header").get<std::string>();
    require(statelessHeader.find("_value_worker(pipelang::model::EvaluationBudget& pl_budget, std::int64_t value)") != std::string::npos,
            "stateless object helper did not lower to a receiver-free worker function");
    const auto refuseWorker = [&](const std::string& name, const std::string& source,
                                  const std::string& expected) {
        oracle.refused("stateless-worker-" + name,
            sourceRequest(source, "stateless_worker_refusal", true), false, expected);
    };
    refuseWorker("direct-state", replace(replace(statelessWorker,
        "public Numeric() {}", "private int bias = 1; public Numeric() {}"),
        "=> Add(value, value);", "=> value + bias;"), "cannot access state");
    refuseWorker("transitive-state", replace(replace(statelessWorker,
        "public Numeric() {}", "private int bias = 1; public Numeric() {}"),
        "=> first + second;", "=> first + second + bias;"), "cannot access state");
    refuseWorker("effect", replace(statelessWorker, "public pure int Twice", "public int Twice"),
        "nonvirtual nongeneric pure method");
    refuseWorker("virtual", replace(statelessWorker, "public pure int Twice", "public virtual pure int Twice"),
        "nonvirtual nongeneric pure method");
    refuseWorker("explicit-receiver", replace(statelessWorker, "Twice(input)", "this.Twice(input)"),
        "owner-confined objects cannot enter parallel workers");
    refuseWorker("captured-object", replace(replace(statelessWorker,
        "parallel foreach", "Numeric other = new Numeric(); parallel foreach"),
        "Twice(input)", "other.Twice(input)"), "owner-confined object references");
    refuseWorker("constructor", replace(statelessWorker,
        "public pure int Twice(int value) => Add(value, value);",
        "public pure int Twice(int value) { Numeric other = new Numeric(); return value; }"),
        "object construction requires owner-confined effect code");
    refuseWorker("generic", replace(replace(statelessWorker,
        "public pure int Twice(int value) => Add(value, value);",
        "public pure T Twice<T>(T value) => value;"), "Twice(input)", "Twice<int>(input)"),
        "nonvirtual nongeneric pure method");
    const auto inheritedWorker = replace(replace(statelessWorker,
        "public Class Numeric {",
        "public Class Base { public pure int Twice(int value) => value + value; } public Class Numeric : Base {"),
        "public pure int Twice(int value) => Add(value, value);", "");
    refuseWorker("inheritance", inheritedWorker, "nonvirtual nongeneric pure method");
    const auto statefulHostWorker = R"pipe(
public Interface Host { pure int Read(int value); }
public Class Numeric {
    private Host host;
    public Numeric(Host supplied) { host = supplied; }
    public pure int Twice(int value) => host.Read(value);
    public void Calculate(List<int> inputs) {
        ConcurrentDictionary<int, int> completed = ConcurrentDictionary<int, int>.Empty();
        parallel foreach (int input in inputs) {
            completed.Set(input, Twice(input));
        }
    }
}
public Class App {
    private Numeric numeric;
    public App(Host supplied) { numeric = new Numeric(supplied); }
    public void Calculate(List<int> inputs) { numeric.Calculate(inputs); }
}
)pipe";
    refuseWorker("host", statefulHostWorker, "cannot access state");
    refuseWorker("shared-handle", replace(statelessWorker,
        "public pure int Twice(int value) => Add(value, value);",
        "public pure int Twice(int value) { Atomic<int> cell = Atomic<int>.Create(0); return cell.Load(); }"),
        "pure methods cannot");

    const auto pureHostValues = oracle.admitted("PureHostValues", sourceRequest(
        "public Interface Math { pure float32 Support(float32 compression, float32 speed); } "
        "public Class App { private Math math; public App(Math service) { math = service; } "
        "public pure float32 Read(float32 value) => math.Support(value, 0f); }", "pure_host_values"));
    require(pureHostValues.at("header").get<std::string>().find("float Support(float compression, float speed) const = 0") != std::string::npos,
        "Pure owned-value host method lost its const native interface");
    require(pureHostValues.at("header").get<std::string>().find(
        "using pipelang::model::validate; auto pl_receiver =") != std::string::npos,
        "Pure owned-value host lowering omitted its receiver and validation scope");
    const std::string pureProjectSource =
        "module PureMath; public Interface Math { pure float32 Support(float32 value); } "
        "public model Class App { private Math math; public App(Math service) { math = service; } "
        "public pure float32 Read(float32 value) => math.Support(value); }";
    const auto pureProject = oracle.admitted("PureHostProject", {
        {"protocol", "pipelang.native-model-project.v1"}, {"entry", "PureMath"},
        {"namespace", "pure_host_project"}, {"modules", Json::array({{
            {"name", "PureMath"}, {"path", "PureMath.pipe"}, {"source", pureProjectSource}}})}});
    require(pureProject.at("types_header").get<std::string>().find("float Support(float value) const = 0") != std::string::npos,
        "Pure host project Types interface lost its const owned-value return");
    const auto refusePureHost = [&](const std::string& name, const std::string& source,
                                   const std::string& expected, bool objects = false) {
        oracle.refused("pure-host-" + name, sourceRequest(source, "pure_host_refusal", objects), false, expected);
    };
    refusePureHost("nonpure-raw", "public Interface Host { int Read(); }", "host interfaces declare synchronous Result");
    refusePureHost("void", "public Interface Host { pure void Read(); }", "pure method requires a synchronous value return");
    refusePureHost("task-raw", "public Interface Host { Task<int> Read(); }", "Task is supported only as Task<Result");
    refusePureHost("task-pure", "public Interface Host { pure Task<Result<int,string>> Read(); }", "pure method requires a synchronous value return");
    refusePureHost("service", "public Interface Service { Result<int,string> Read(); } public Interface Host { pure Service Read(); }", "service references");
    refusePureHost("atomic", "public Interface Host { pure Atomic<int> Read(); }", "service references");
    refusePureHost("nullable-service", "public Interface Service { Result<int,string> Read(); } public Interface Host { pure Service? Read(); }", "owned value payload");
    const std::string reference = "public Class Value { public pure int Read() => 1; } ";
    refusePureHost("reference", reference + "public Interface Host { pure Value Read(); } public Class App {}", "deeply owned values", true);
    refusePureHost("record-reference", reference + "public Record Box { public Value value; } public Interface Host { pure Box Read(); } public Class App {}", "records contain owned value fields only", true);
    refusePureHost("argument-reference", reference + "public Record Box { public Value value; } public Interface Host { pure int Read(Box value); } public Class App {}", "records contain owned value fields only", true);
    refusePureHost("parallel", "public Interface Host { pure float32 Read(float32 value); } public Class App { private Host host; public App(Host service) { host = service; } public void Run(List<float32> values) { parallel foreach (float32 value in values) { float32 result = host.Read(value); } } }", "parallel workers cannot access host services");
    oracle.admitted("PureHostNestedVoidResult", sourceRequest(
        "public Interface Host { pure List<Result<void,string>> Read(); }", "pure_host_nested"));
    std::string sharedRecords = "public Record R0 { public int value; } ";
    for (int index = 1; index <= 30; ++index) {
        const auto child = "R" + std::to_string(index - 1);
        sharedRecords += "public Record R" + std::to_string(index) + " { public " + child +
            " first; public " + child + " second; } ";
    }
    oracle.refused("pure-host-shared-record-budget", sourceRequest(sharedRecords +
        "public Interface Host { pure R30 Read(); }", "pure_host_record_budget"), false,
        "expanded value layout limit exceeded");
    oracle.admitted("PureHostSharedRecord", sourceRequest(
        "public Record Leaf { public int value; } "
        "public Record Pair { public Leaf first; public Leaf second; } "
        "public Interface Host { pure Pair Read(); }", "pure_host_shared_record"));
    const auto nullable = [&](const std::string& name, const std::string& body,
                              const std::string& expected) {
        oracle.refused("nullable-" + name, sourceRequest("public Class App { " + body + " }", "nullable_refusal"), false, expected);
    };
    nullable("old-type", "public Optional<int> Value;", "replaced by T?");
    nullable("old-constructor", "public pure int? Read() => Optional<int>.None();", "replaced by T?");
    nullable("suffix-constructor", "public pure int? Read() => int?.Some(1);", "typed values or null");
    nullable("null-into-int", "public pure int Read() => null;", "expected int, got null");
    nullable("null-into-record", "public int Value = null;", "expected int, got null");
    nullable("no-unwrap", "public pure int Read(int? value) => value;", "expected int, got int?");
    nullable("wrong-payload", "public pure string? Read() => 1;", "expected string?, got int");
    nullable("untyped-null-equality", "public pure bool Read() => null == null;", "nullable target type");
    nullable("malformed-suffix", "public int?<> Value;", "expected identifier");
    nullable("nullable-void", "public void? Value;", "void is not a value type");
    nullable("null-argument", "private pure int Read(int value) => value; public pure int Invoke() => Read(null);", "no exact matching method overload");
    nullable("ambiguous-null", "private pure int Read(int? value) => 1; private pure int Read(string? value) => 2; public pure int Invoke() => Read(null);", "ambiguous nullable method overload");
    nullable("suffix-depth", "public int????????????? Value;", "type nesting limit exceeded");
    nullable("mixed-depth", "public List<int??????????" ">?? Value;", "type nesting limit exceeded");
    oracle.admitted("NullableDepthBoundary", sourceRequest(
        "public Record Boundary { public List<int??????????" ">? Value; }", "nullable_boundary"));
    const std::string oldNullableSource = "public Class App { public Optional<int> Value; }";
    const auto oldNullableDiagnostic = oracle.invoke(sourceRequest(oldNullableSource, "nullable_origin"));
    require(!oldNullableDiagnostic.at("ok").get<bool>()
        && oldNullableDiagnostic.at("diagnostics").at(0).at("offset") == oldNullableSource.find("Optional"),
        "Old nullable syntax lost its exact authored token offset");
    const auto nullableSyntax = oracle.admitted("NullableSyntax", sourceRequest(
        "public Record Box<T> { public T? Value; } "
        "public Class App { public Box<int?> Nested; "
        "public pure Result<float32?,string> Sample() => Result<float32?,string>.Success(null); "
        "public pure int? Spacing(bool choose) => choose ? 3 : null; "
        "public pure int? BothAbsent(bool choose) => choose ? null : null; "
        "public pure int?? Layers() { int? inner = null; return inner; } }", "nullable_syntax"));
    require(nullableSyntax.at("header").get<std::string>().find("$null") == std::string::npos,
        "Internal untyped null marker escaped into generated C++");
    oracle.refused("nullable-nominal-display", sourceRequest(
        "public Record MyOptional<T> { public T Value; } "
        "public Class App { public MyOptional<int> Value = 0; }", "nullable_nominal"),
        false, "expected MyOptional<int>, got int");
    oracle.refused("nullable-nominal-nested-display", sourceRequest(
        "public Record MyOptional<T> { public T Value; } "
        "public Class App { public MyOptional<int?> Value = 0; }", "nullable_nominal_nested"),
        false, "expected MyOptional<int?>, got int");
    const auto byteBufferSource = oracle.source("tests/native-models/ByteBufferPacket.pipe") + "\n" +
        oracle.source("tests/native-models/ByteBufferHost.pipe") + "\n" +
        oracle.source("tests/native-models/ByteBuffers.pipe");
    oracle.admitted("ByteBuffers", sourceRequest(byteBufferSource, "byte_buffers"));
    const auto byteBufferObjects = oracle.source("tests/native-models/ByteBufferPacket.pipe") + "\n" +
        oracle.source("tests/native-models/ByteBufferBox.pipe") + "\n" +
        oracle.source("tests/native-models/ByteBufferOwner.pipe");
    oracle.admitted("ByteBufferObjects", sourceRequest(byteBufferObjects, "byte_buffer_objects", true));
    const auto byteBufferProjectSource = "module Binary;\n" + replace(byteBufferObjects,
        "public Class App", "public model Class App");
    oracle.admitted("ByteBufferProject", Json{
        {"protocol", "pipelang.native-model-project.v1"}, {"entry", "Binary"},
        {"namespace", "byte_buffer_project"}, {"modules", Json::array({
            Json{{"name", "Binary"}, {"path", "Binary.pipe"}, {"source", byteBufferProjectSource}}
        })}
    });
    oracle.refused("bytebuffer-implicit-text", sourceRequest(
        "public Class App { public pure ByteBuffer Read() => \"bytes\"; }", "buffer_text"));
    oracle.refused("bytebuffer-add-wrong-type", sourceRequest(
        "public Class App { public void Read() { ByteBuffer value; value.Add(1); } }", "buffer_add"));
    oracle.refused("bytebuffer-slice-wrong-type", sourceRequest(
        "public Class App { public pure ByteBuffer Read(ByteBuffer value) => value.Slice(0u, 1); }", "buffer_slice"));
    oracle.refused("bytebuffer-pure-field-mutation", sourceRequest(
        "public Class App { public ByteBuffer value; public pure void Read() { value.Add(1u); } }", "buffer_pure"));
    oracle.refused("bytebuffer-unknown-constructor", sourceRequest(
        "public Class App { public pure ByteBuffer Read() => ByteBuffer.FromBytes(\"x\"); }", "buffer_constructor"));
    oracle.refused("bytebuffer-generic", sourceRequest(
        "public Class App { public ByteBuffer<int> value; }", "buffer_generic"));
    const auto foundation = oracle.admitted("Foundation", sourceRequest(
        oracle.source("tests/native-models/Foundation.pipe"), "foundation"));
    require(foundation.at("constants_header").get<std::string>().find("pipelang/model") == std::string::npos &&
            foundation.at("manifest").at("constants").size() >= 12,
        "typed standalone constants lost their exports or acquired runtime headers");
    oracle.admitted("FoundationObjects", sourceRequest(
        oracle.source("tests/native-models/FoundationObjects.pipe"), "foundation_objects", true));

    oracle.admitted("UInt32Objects", sourceRequest(
        oracle.source("tests/native-models/UInt32Objects.pipe"), "uint32_objects", true));
    oracle.admitted("SyncHostObjects",
 sourceRequest(
        oracle.source("tests/native-models/SyncHostObjects.pipe"), "sync_host_objects", true));
    oracle.admitted("InitializationObjects", sourceRequest(
        oracle.source("tests/native-models/InitializationObjects.pipe"), "initialization_objects", true));
    oracle.admitted("ListReadObjects", sourceRequest(
        oracle.source("tests/native-models/ListReadObjects.pipe"), "pipelang_list_read_objects", true));
    const std::string constantPrefix = "public Class App { ";
    const auto refuseConstant = [&](const std::string& name, const std::string& body, const std::string& expected) {
        oracle.refused("constant-" + name, sourceRequest(constantPrefix + body + " }", "constant_refusal"), false, expected);
    };
    refuseConstant("missing-initializer", "public const int Value;", "expected '='");
    refuseConstant("owned-type", "public const string Value = \"text\";", "const requires bool, int, uint32, float or float32");
    refuseConstant("state-reference", "public int value = 1; public const int Value = value;", "cannot reference model state");
    refuseConstant("arbitrary-call", "public pure int Compute() => 3; public const int Value = Compute();", "closed scalar values");
    refuseConstant("cycle", "public const int First = Second; public const int Second = First;", "constant dependency cycle");
    refuseConstant("signed-overflow", "public const int Value = 9223372036854775807 + 1;", "invalid constant");
    refuseConstant("unsigned-underflow", "public const uint32 Value = 0u - 1u;", "invalid constant");
    refuseConstant("unsigned-overflow", "public const uint32 Value = 4294967295u + 1u;", "invalid constant");
    refuseConstant("divide-zero", "public const int Value = 1 / 0;", "invalid constant");
    refuseConstant("remainder-zero", "public const int Value = 1 % 0;", "invalid constant");
    refuseConstant("divide-minimum", "public const int Value = -9223372036854775808 / -1;", "invalid constant");
    refuseConstant("shift-negative", "public const uint32 Value = 1u << -1;", "invalid constant");
    refuseConstant("shift-high", "public const uint32 Value = 1u >> 32;", "invalid constant");
    refuseConstant("conversion-negative", "public const uint32 Value = uint32.FromInt(-1);", "invalid constant");
    refuseConstant("conversion-nan", "public const int Value = int.FromFloat(float.NaN);", "invalid constant");
    refuseConstant("mutation", "public const int Value = 1; public void Change() { Value = 2; }", "cannot assign to const member");
    refuseConstant("mixed-width", "public const float32 Value = 1.0f + 2.0;", "expected float32");
    oracle.refused("constant-private-access", sourceRequest(
        "public Class Values { private const int Hidden = 1; } public Class App { public const int Value = Values.Hidden; }",
        "constant_private"), false, "constant is private");
    oracle.refused("constant-instance-access", sourceRequest(
        "public Class Cell { public const int Value = 1; } public Class App { private Cell cell = new Cell(); public pure int Read() => cell.Value; }",
        "constant_instance", true), false, "requires its class name");
    oracle.refused("constant-inherited-field", sourceRequest(
        "public Class Base { public const int Value = 1; } public Class Child : Base { public int Value = 2; } public Class App { }",
        "constant_inheritance", true), false, "inherited constants cannot be hidden");
    oracle.refused("constant-namespace-collision", sourceRequest(
        "public Class constants { } public Class App { public const int Value = 1; }", "constant_namespace"),
        false, "constant export namespace conflicts");
    const Json importedConstantFailure{{"protocol", "pipelang.native-model.v2"}, {"entry", "App"},
        {"namespace", "constant_origin"}, {"modules", {
            {{"name", "App"}, {"source", "module App; import Values; public Class App { public const int Value = Numbers.Bad; }"}},
            {{"name", "Values"}, {"source", "module Values; public Class Numbers { public const int Bad = 1 / 0; }"}}
        }}};
    oracle.refused("constant-imported-diagnostic", importedConstantFailure, false, "invalid constant");
    require(oracle.invoke(importedConstantFailure).at("diagnostics")[0].at("module") == "Values",
        "folded constant diagnostic lost its defining source module");
    oracle.refused("constant-inherited-method", sourceRequest(
        "public Class Base { public pure int Read() => 1; } public Class Child : Base { public const int Read = 2; } public Class App { }",
        "constant_method", true), false, "cannot be hidden");
    const Json constantNamespaceConflict{{"protocol", "pipelang.native-model.v2"}, {"entry", "App"},
        {"namespace", "constant_namespace_path"}, {"modules", {
            {{"name", "App"}, {"source", "module App; import Values; import Values.Root; public Class App { }"}},
            {{"name", "Values"}, {"source", "module Values; public Class Root { public const int Child = 1; }"}},
            {{"name", "Values.Root"}, {"source", "module Values.Root; public Class Child { public const int Value = 2; }"}}
        }}};
    oracle.refused("constant-module-value-namespace", constantNamespaceConflict, false, "constant export name conflicts");
    const std::string pureHost = "public Interface Math { pure Result<int,string> Read(int value); } ";
    const std::string injected = "public Class App { private Math math; public App(Math query) { math = query; } ";
    oracle.refused("pure-host-transitive-worker", sourceRequest(pureHost + injected +
        "private pure int Get(int value) => math.Read(value).Value; public void Run(List<int> values) { parallel foreach (int value in values) { int result = Get(value); } } }",
        "pure_worker"), false, "parallel helper cannot access host services");
    oracle.refused("pure-host-effectful-call", sourceRequest(
        "public Interface Math { Result<int,string> Height(); } public Class App { private Math math; public App(Math query) { math = query; } public pure Result<int,string> Read() => math.Height(); }",
        "pure_effect"), false, "pure methods cannot call effectful host services");
    oracle.refused("prefix-wrong-type", sourceRequest(
        "public Class App { public pure bool Read() => string.StartsWith(1, \"x\"); }", "prefix_type"));
    oracle.refused("prefix-wrong-arity", sourceRequest(
        "public Class App { public pure bool Read() => string.StartsWith(\"x\"); }", "prefix_arity"));
    oracle.refused("utf8-length-wrong-type", sourceRequest(
        "public Class App { public pure int Read() => string.Utf8Length(1); }", "utf8_length_type"));
    oracle.refused("utf8-length-wrong-arity", sourceRequest(
        "public Class App { public pure int Read() => string.Utf8Length(); }", "utf8_length_arity"));
    oracle.refused("utf8-slice-wrong-type", sourceRequest(
        "public Class App { public pure string Read() => string.Utf8Slice(\"x\", 0u, 1); }", "utf8_slice_type"));
    oracle.refused("utf8-slice-wrong-arity", sourceRequest(
        "public Class App { public pure string Read() => string.Utf8Slice(\"x\", 0); }", "utf8_slice_arity"));
    oracle.refused("utf8-search-wrong-pattern", sourceRequest(
        "public Class App { public pure int Read() => string.Utf8IndexOf(\"x\", 1, 0); }", "utf8_search_type"));
    oracle.refused("utf8-search-wrong-start", sourceRequest(
        "public Class App { public pure int Read() => string.Utf8IndexOf(\"x\", \"x\", false); }", "utf8_search_start"));
    oracle.refused("utf8-search-wrong-arity", sourceRequest(
        "public Class App { public pure int Read() => string.Utf8IndexOf(\"x\", \"x\"); }", "utf8_search_arity"));
    oracle.refused("integer-text-wrong-type", sourceRequest(
        "public Class App { public pure string Read() => string.FromInt(1u); }", "integer_text_type"));
    oracle.refused("integer-text-wrong-arity", sourceRequest(
        "public Class App { public pure string Read() => string.FromInt(1, 2); }", "integer_text_arity"));
    oracle.refused("ieee-unknown-name", sourceRequest(
        "public Class App { public pure float32 Read() => float32.Infinity; }", "ieee_name"), false, "unknown typed IEEE constant");
    const auto data = [&](const std::string& name) { return oracle.source("tests/native-models/data/" + name + ".pipe"); };
    for (const auto& [name, space] : std::map<std::string, std::string>{
        {"Collections", "pipelang_models"}, {"Parallel", "pipelang_models"}, {"Synchronization", "synchronization_models"},
        {"Objects", "object_models"}, {"ObjectComposition", "object_composition"}, {"InterfaceInheritance", "interface_inheritance"}}) {
        oracle.admitted(name, sourceRequest(oracle.source("tests/native-models/" + name + ".pipe"), space,
            name == "Objects" || name == "ObjectComposition" || name == "InterfaceInheritance"));
    }
    oracle.admitted("Composition", sourceRequest(data("composition-source"), "composition"));
    oracle.admitted("InitializerOverloads", sourceRequest(data("composition-initializer_source"), "initializers"));
    const auto modules = [](const std::string& space, const std::string& entry, const Json& sources, bool objects = false) {
        Json request{{"protocol", objects ? "pipelang.native-model.v3" : "pipelang.native-model.v2"},
            {"namespace", space}, {"entry", entry}, {"modules", sources}};
        if (objects)
            request["models"] = {"App"};
        return request;
    };
    oracle.admitted("Modules", modules("modules", "App", {
        {{"name", "Values"}, {"source", data("composition-library")}}, {{"name", "App"}, {"source", data("composition-application")}}}));
    oracle.admitted("Mixed", modules("mixed", "App", {
        {{"name", "Data"}, {"source", data("mixed-DATA")}}, {{"name", "App"}, {"source", data("mixed-APP")}}}));
    oracle.admitted("Concurrent", modules("concurrent_models", "App", {
        {{"name", "Values"}, {"source", "module Values; public Record Box<T> { public T Value; }"}},
        {{"name", "App"}, {"source", oracle.source("tests/native-models/Concurrent.pipe")}}}));
    oracle.admitted("ObjectModules", modules("object_modules", "Entry", {
        {{"name", "Values"}, {"source", data("objects_source-Values")}}, {{"name", "Entry"}, {"source", data("objects_source-Entry")}}}, true));
    oracle.admitted("InterfaceModules", modules("interface_modules", "Entry", {
        {{"name", "BaseTypes"}, {"source", data("interfaces_source-BaseTypes")}},
        {{"name", "Types"}, {"source", data("interfaces_source-Types")}},
        {{"name", "Entry"}, {"source", data("interfaces_source-Entry")}}}, true));
    const auto refusals = parseJson(oracle.source("tests/native-models/data/semantic-refusals.json"), 1U << 20);
    for (const auto& item : refusals)
        oracle.refused(item.at("id"), item.at("request"));
    for (const auto& name : {"Objects", "InterfaceInheritance"})
        oracle.refused(std::string(name) + "/legacy-protocol", sourceRequest(oracle.source("tests/native-models/" + std::string(name) + ".pipe"), "boundary"));
    oracle.refused("module/missing-import", modules("boundary", "App", {{{"name", "App"}, {"source", "module App; import Missing; public Class App {}"}}}));
    oracle.refused("module/duplicate", modules("boundary", "App", {
        {{"name", "App"}, {"source", "module App; public Class App {}"}}, {{"name", "App"}, {"source", "module App; public Class App {}"}}}));

    const auto model = oracle.admitted("FixtureModel", sourceRequest(oracle.source("tests/native-model-views/FixtureModel.pipe"), "pipelang_models"));
    const auto source = oracle.source("tests/native-model-views/Fixture.pview");
    Json view{{"protocol", "pipelang.native-model-view.v1"}, {"source", source}, {"namespace", "pipelang_models"},
        {"model_header", "Model.hpp"}, {"model_manifest", model.at("manifest")}};
    const auto plain = oracle.admitted("View", view, true);
    pipelang::tests::nativeXmlViewTests(oracle, view, plain);
    for (const std::string type : {"uint32", "List<uint32>", "Result<uint32,string>"}) {
        auto unsupported = view;
        unsupported["model_manifest"]["classes"][0]["fields"].push_back({{"name", "unsignedWord"}, {"type", type}});
        oracle.refused("view/unsupported-" + type, unsupported, true, "view bindings do not support uint32 values");
    }

    std::size_t mutation = 0;
    for (const auto& [old, value] : std::vector<std::pair<std::string, std::string>>{
        {"model.Draft.Name", "model.Draft.Missing"}, {"model.Draft.Name", "model.Saving"},
        {"model.CanSave()", "model.Fail()"}, {"changed: model.SetName", "clicked: model.SetName"},
        {"clicked: model.Save", "changed: model.Save"}, {"clicked: model.Save", "clicked: model.SetName"},
        {"model: FixtureModel", "alias: FixtureModel"}, {"id: \"save\"", "id: \"name\""},
        {"view FixtureView", "view class"}, {"changed: model.SetName", "changed: model.SetName, changed: model.SetName"}}) {
        auto candidate = view;
        candidate["source"] = replace(source, old, value);
        oracle.refused("view/mutation-" + std::to_string(mutation++), candidate, true);
    }
    auto styled = view;
    styled["source"] = oracle.source("tests/native-model-views/Theme.pview") + replace(source, "FixtureModel) {", "FixtureModel) style Compact {");
    const auto themed = oracle.admitted("StyledView", styled, true);
    require(themed.at("manifest") == plain.at("manifest"), "style changed view identity");
    styled["style_inspection"] = "pipelang.native-style-descriptor.v1";
    auto inspected = oracle.admitted("InspectedView", styled, true);
    const auto descriptor = inspected.at("style_descriptor");
    require(descriptor.at("style") == "Compact" && descriptor.at("scope") == "resolved-authored" && descriptor.at("properties").size() == 7,
        "resolved style descriptor differs");
    inspected.erase("style_descriptor");
    require(inspected == themed, "inspection changed default wire response");
    const auto styleRefusals = parseJson(oracle.source("tests/native-model-views/style-refusals.json"));
    for (std::size_t index = 0; index < styleRefusals.size(); ++index) {
        auto candidate = view;
        candidate["source"] = styleRefusals[index].at("source").get<std::string>() + "\n" + source;
        oracle.refused("style/refusal-" + std::to_string(index), candidate, true);
    }
    for (const Json& selector : Json::array({false, true, nullptr, 1, Json::array(), Json::object(), "", "pipelang.native-style-descriptor.v2"})) {
        auto candidate = view;
        candidate["style_inspection"] = selector;
        oracle.refused("style/invalid-selector-" + selector.dump(), candidate, true);
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--numeric-guard") {
            using namespace pipelang::tools;
            for (const auto& flag : {"", "-ffast-math", "-ffinite-math-only"}) {
                std::vector<std::string> command{nativeTool("c++").string(), "-std=c++20", "-fsyntax-only", "-x", "c++",
                    "-I" + (fs::path(argv[2]) / "include").string(), argv[3]};
                if (*flag)
                    command.push_back(flag);
                const auto result = process(command, {}, std::chrono::seconds(60), 1U << 20);
                if (*flag)
                    require(result.exit != 0 && result.diagnostic.find("require strict IEEE") != std::string::npos, "unsafe floating mode was not refused");
                else
                    require(result.exit == 0, "strict floating mode failed: " + result.diagnostic);
            }
            std::cout << "strict IEEE compilation passed; unsafe modes refused\n";
            return 0;
        }
        pipelang::tools::require(argc == 4, "usage: extended-tests SOURCE_ROOT SDK OUTPUT");
        Oracle oracle(argv[1], argv[2], argv[3]);
        run(oracle);
        oracle.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extended compiler oracle: " << error.what() << '\n';
        return 1;
    }
}
