// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/composition.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pipelang::models;
using Json = nlohmann::json;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& source) {
    std::ofstream output(path);
    output << source;
    require(output.good(), "cannot write " + path.string());
}

Program checked(const std::string& source) {
    auto program = parse(source, true);
    program.objectEnabled = true;
    program.modelExports = {"App"};
    normalizeAndCheckComposition(program);
    return program;
}

void visitStatements(const std::vector<Statement>& statements,
                     const std::function<void(const Statement&)>& visitor) {
    for (const auto& statement : statements) {
        visitor(statement);
        visitStatements(statement.initialization, visitor);
        visitStatements(statement.body, visitor);
        visitStatements(statement.iteration, visitor);
        visitStatements(statement.alternative, visitor);
        visitStatements(statement.cleanup, visitor);
    }
}

void requireConcreteLocals(const Program& program) {
    for (const auto& declaration : program.declarations) {
        for (const auto& method : declaration.methods) {
            visitStatements(method.body, [](const Statement& statement) {
                if (statement.kind == "local" || statement.kind == "foreach" ||
                    statement.kind == "parallel_foreach") {
                    require(!statement.type.name.empty() && statement.type.name != "var" &&
                        statement.type.name != "$null" && statement.type.name != "void",
                        "inference left an unresolved or non-value type in the checked AST");
                }
            });
        }
    }
}

void equivalent(const std::string& explicitSource, const std::string& inferredSource,
                const std::string& purpose, bool legacy = false) {
    const auto explicitResult = legacy ? compile(explicitSource, "var_equivalence")
        : compileObjects(explicitSource, {"App"}, "var_equivalence");
    const auto inferredResult = legacy ? compile(inferredSource, "var_equivalence")
        : compileObjects(inferredSource, {"App"}, "var_equivalence");
    require(explicitResult == inferredResult, "explicit/inferred emitted contract diverged: " + purpose);
    if (!legacy)
        requireConcreteLocals(checked(inferredSource));
}

void local(const std::string& type, const std::string& initializer,
           const std::string& declarations = {}, const std::string& parameters = {}) {
    const auto prefix = declarations + " public Class App { public void Check(" + parameters + ") { ";
    const auto suffix = " value = " + initializer + "; } }";
    equivalent(prefix + type + suffix, prefix + "var" + suffix, type);
}

void valueContracts() {
    const std::vector<std::pair<std::string, std::string>> primitives{
        {"bool", "true"}, {"int", "42"}, {"uint32", "42u"}, {"float", "1.5"},
        {"float32", "1.5f"}, {"string", "\"text\""}, {"Error", "Error(\"error\")"},
        {"ByteBuffer", "ByteBuffer.FromUtf8(\"bytes\")"},
        {"List<int>", "List<int>.Empty()"},
        {"Array<int>", "Array<int>.Create(2, 0)"},
        {"FixedArray<int,2>", "FixedArray<int,2>.Create(3)"},
        {"Dictionary<string,int>", "Dictionary<string,int>.Empty()"},
        {"HashSet<int>", "HashSet<int>.Empty()"},
        {"Queue<int>", "Queue<int>.Empty()"}, {"Stack<int>", "Stack<int>.Empty()"},
        {"Result<int,string>", "Result<int,string>.Success(42)"},
        {"Result<void,string>", "Result<void,string>.Failure(\"failure\")"},
        {"Result<List<int>,string>", "Result<List<int>,string>.Success(List<int>.Empty())"},
        {"Atomic<int>", "Atomic<int>.Create(0)"},
        {"ConcurrentDictionary<string,int>", "ConcurrentDictionary<string,int>.Empty()"},
        {"ConcurrentQueue<int>", "ConcurrentQueue<int>.Empty()"},
        {"ConcurrentStack<int>", "ConcurrentStack<int>.Empty()"},
        {"Lock", "Lock.Create()"}, {"SemaphoreSlim", "SemaphoreSlim.Create(1, 1)"}
    };
    for (const auto& [type, initializer] : primitives)
        local(type, initializer);

    local("Pair", "Pair(42)", "public Record Pair { public int value; }");
    local("Shade", "Shade.Red", "public Enum Shade { Red, Blue }");
    local("Other", "new Other()", "public Class Other {}");
    local("Box<int>", "new Box<int> { value = 42 }", "public Record Box<T> { public T value; }");
    local("int?", "maybe", {}, "int? maybe");
    local("List<int?>", "List<int?>.Empty()");

    equivalent(R"pipe(public Class App {
        private pure T Echo<T>(T value) => value;
        public pure int Check() { int value = Echo<int>(42); return value; }
    })pipe", R"pipe(public Class App {
        private pure T Echo<T>(T value) => value;
        public pure int Check() { var value = Echo<int>(42); return value; }
    })pipe", "generic helper result");

    equivalent(R"pipe(public Class App {
        private pure int? Maybe() => null;
        public pure bool Check() { int? value = Maybe(); return value == null; }
    })pipe", R"pipe(public Class App {
        private pure int? Maybe() => null;
        public pure bool Check() { var value = Maybe(); return value == null; }
    })pipe", "nullable function result");

    equivalent("public Class App { public pure int Read() { int value = 42; return value; } }",
        "public Class App { public pure int Read() { var value = 42; return value; } }", "v1 local", true);
    const auto explicitModules = compileModules({{"Legacy", R"pipe(module Legacy;
        public Class App { public pure int Read() { int value = 42; return value; } }
    )pipe"}}, "Legacy", "var_v2");
    const auto inferredModules = compileModules({{"Legacy", R"pipe(module Legacy;
        public Class App { public pure int Read() { var value = 42; return value; } }
    )pipe"}}, "Legacy", "var_v2");
    require(explicitModules == inferredModules, "v2 inference changed emitted compatibility contracts");
}

void loopAndAwaitContracts() {
    const std::string typed = R"pipe(public Class App {
        public pure int Read(ByteBuffer bytes, Dictionary<string,int> mapping) {
            int total = 0;
            for (int index = 0; index < 2; index = index + 1) { total = total + index; }
            foreach (uint32 octet in bytes) { total = total + int.FromUInt32(octet); }
            foreach (KeyValuePair<string,int> pair in mapping) { total = total + pair.Value; }
            return total;
        }
    })pipe";
    auto inferred = typed;
    for (const auto& marker : {"int total", "int index", "uint32 octet", "KeyValuePair<string,int> pair"}) {
        const std::string text(marker);
        inferred.replace(inferred.find(text), text.rfind(' '), "var");
    }
    equivalent(typed, inferred, "for/foreach collection element inference");

    equivalent(R"pipe(public Class App {
        public int Run(List<int> values) {
            Atomic<int> total = Atomic<int>.Create(0);
            parallel foreach (int item in values) { int added = total.Add(item); }
            return total.Load();
        }
    })pipe", R"pipe(public Class App {
        public int Run(List<int> values) {
            var total = Atomic<int>.Create(0);
            parallel foreach (var item in values) { var added = total.Add(item); }
            return total.Load();
        }
    })pipe", "parallel item/local/capture inference");

    const std::string asyncTyped = R"pipe(public Interface Host {
        Task<Result<int,string>> Read(CancellationToken token);
    }
    public Class App {
        private Host host;
        public App(Host host) { this.host = host; }
        public async Task<Result<int,string>> Run(CancellationToken token) {
            Result<int,string> result = await host.Read(token);
            return result;
        }
    })pipe";
    auto asyncInferred = asyncTyped;
    asyncInferred.replace(asyncInferred.find("Result<int,string> result"), std::string("Result<int,string>").size(), "var");
    equivalent(asyncTyped, asyncInferred, "awaited Result exact inferred type");
}

void refused(const std::string& source, const std::string& expected) {
    try {
        (void)compileObjects(source, {"App"}, "var_refusal");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(expected) != std::string::npos,
            "expected " + expected + ", got " + diagnostic.what());
        require(diagnostic.offset < source.size(), "inference diagnostic lost its source offset");
        return;
    }
    throw std::runtime_error("invalid inference accepted: " + expected);
}

void refusalContracts() {
    const std::vector<std::pair<std::string, std::string>> bodies{
        {"var value;", "require an initializer"},
        {"var value = null;", "cannot infer var from null"},
        {"var value = value;", "own initializer"},
        {"var value = 1; value = \"bad\";", "expected int, got string"},
        {"var value = 1; var value = 2;", "unique unshadowed name"},
        {"{ var value = 1; } int copy = value;", "unknown value"},
        {"for (var index = 0; index < 1; index = index + 1) {} int copy = index;", "unknown value"},
        {"foreach (var item in List<int>.Empty()) {} int copy = item;", "unknown value"},
        {"for (var index = 0; index < 1; var next = 1) {}", "not allowed in this statement context"},
        {"var value = 1; parallel foreach (var item in List<int>.Empty()) { value = item; }", "parallel"},
        {"parallel foreach (var item in ByteBuffer.Empty()) {}", "does not support ByteBuffer"},
        {"var token = CancellationToken.None;", "owned value"}
    };
    for (const auto& [body, diagnostic] : bodies)
        refused("public Class App { public void Run() { " + body + " } }", diagnostic);

    refused("public Class App { public var value; }", "API types must be explicit");
    refused("public Record Record { public var value; } public Class App {}", "API types must be explicit");
    refused("public Class App { public var Run() => 1; }", "API types must be explicit");
    refused("public Class App { public int Run(var value) => 1; }", "API types must be explicit");
    refused("public Class App { public void Run() { try {} catch (var error) {} } }", "API types must be explicit");
    refused("public Class App { public void Run() { var value = List<var>.Empty(); } }", "API types must be explicit");
    refused("public Class App { private void Nothing() {} public void Run() { var value = Nothing(); } }",
        "non-void static type");
    refused(R"pipe(public Class App {
        private int value = 0;
        public void Run() { var value = 1; }
    })pipe", "unique unshadowed name");
    refused(R"pipe(public Class Object {}
    public Class App {
        public void Run(List<Object> values) {
            parallel foreach (var item in values) {}
        }
    })pipe", "owner-confined object references");
    refused(R"pipe(public Class Object { public pure int Read() => 1; }
    public Class App {
        public void Run() {
            var object = new Object();
            parallel foreach (var item in List<int>.Empty()) { var value = object.Read(); }
        }
    })pipe", "owner-confined");
    refused(R"pipe(public Interface Host { Task<Result<int,string>> Read(); }
    public Class App {
        private Host host;
        public App(Host host) { this.host = host; }
        public async Task<Result<int,string>> Run() {
            var pendingTask = host.Read();
            return Result<int,string>.Success(1);
        }
    })pipe", "await");
}

void contextualContracts() {
    equivalent("public Class App { public pure int Run(int var) { int copy = var; var = copy + 1; return var; } }",
        "public Class App { public pure int Run(int var) { var copy = var; var = copy + 1; return var; } }",
        "existing explicit var identifier");
    const auto nominal = compileObjects(
        "public Record var { public int value; } public Class App { public pure int Run() { var item = var(7); return item.value; } }",
        {"App"}, "contextual_var");
    require(nominal.at("ok") == true, "existing named var type was not preserved");
    const std::vector<ModuleSource> modules{{"Named", "module Named; public Record var { public int value; } public model Class App { public pure int Run() { var item = var(7); return item.value; } }", "Named.pipe"}};
    require(compileProjectModules(modules, "Named", "contextual_var_project").at("ok") == true,
        "project named var type did not use the owning resolver");
    const auto namedAnalysis = analyzeProjectModules(modules, "Named");
    for (const auto& symbol : namedAnalysis.at("symbols"))
        require(symbol.at("kind") != "inferred-type",
            "explicit nominal var type was mislabeled as inferred syntax");
}

void semanticContracts() {
    const std::string utility = R"pipe(module Shared;
public Class Utility {
    public static pure int Read() { var count = 2; return count; }
})pipe";
    const std::string extra = R"pipe(module Shared;
public Class Extra {
    public static pure int Read() { var count = 3; return count; }
})pipe";
    const std::string source = R"pipe(module Inference;
import Shared;
// café 😀: symbol positions below are UTF-8 byte offsets.
public model Class App {
    public pure int Run() {
        var count = Utility.Read() + Extra.Read();
        {
            var nested = 2;
            count = count + nested;
        }
        for (var index = 0; index < 2; index = index + 1) {
            var copy = index;
            count = count + copy;
        }
        return count;
    }
})pipe";
    const std::vector<ModuleSource> modules{
        {"Shared", utility, "lib/Utility.pipe"}, {"Shared", extra, "lib/Extra.pipe"},
        {"Inference", source, "app/App.pipe"}};
    const auto index = indexProjectSources(modules);
    require(index.at("ok") == true && index.at("fragments").size() == 3,
        "compiler-owned header index lost source fragments");
    require(index.at("fragments").at(2).at("name") == "Inference" &&
        index.at("fragments").at(2).at("imports") == Json::array({"Shared"}),
        "compiler-owned header index lost module/import identities");
    const auto implicit = indexProjectSources({{"", "public Class Value {}", "src/Implicit.pipe"}});
    require(implicit.at("fragments").at(0).at("name") == "Implicit",
        "implicit editor module identity diverged from project compiler");
    const auto analysis = analyzeProjectModules(modules, "Inference");
    require(analysis.at("ok") == true && analysis.at("diagnostics").empty(),
        "semantic inference analysis failed");
    require(!analysis.contains("header") && !analysis.contains("implementation"),
        "analysis unexpectedly generated a native backend artifact");

    const std::map<std::string, std::string> sources{
        {"lib/Utility.pipe", utility}, {"lib/Extra.pipe", extra}, {"app/App.pipe", source}};
    std::map<std::string, std::size_t> countDeclarations;
    std::size_t inferredKeywords = 0;
    bool nestedScope = false;
    bool forScope = false;
    for (const auto& symbol : analysis.at("symbols")) {
        const auto path = symbol.at("path").get<std::string>();
        const auto offset = symbol.at("offset").get<std::size_t>();
        const auto length = symbol.at("length").get<std::size_t>();
        const auto name = symbol.at("name").get<std::string>();
        require(sources.at(path).substr(offset, length) == name,
            "semantic symbol lost exact UTF-8 source bytes or origin");
        if (symbol.at("kind") == "method")
            require(name != "module", "synthetic default constructor produced a bogus module method symbol");

        if (symbol.at("kind") == "inferred-type") {
            require(name == "var" && symbol.at("type") == "int", "var hover lost its inferred type");
            ++inferredKeywords;
        }
        if (name == "count" && symbol.at("declaration") == true)
            ++countDeclarations[path];

        if (path != "app/App.pipe")
            continue;

        if (name == "nested" && symbol.at("declaration") == true) {
            require(symbol.at("scope_end").get<std::size_t>() <= source.find("for ("),
                "nested local symbol escaped its lexical block");
            nestedScope = true;
        }
        if (name == "index" && symbol.at("declaration") == true) {
            require(symbol.at("scope_end").get<std::size_t>() <= source.find("return count"),
                "for initializer symbol escaped its loop");
            forScope = true;
        }
        if (symbol.at("kind") == "local" && symbol.at("declaration") == false) {
            const auto declaration = symbol.at("declaration_offset").get<std::size_t>();
            require(source.substr(declaration, name.size()) == name,
                "local reference lost its declaration identity");
            require(symbol.at("declaration_path") == path, "local definition crossed source fragments");
        }
    }
    require(inferredKeywords == 6 && nestedScope && forScope,
        "semantic analysis lost var keywords or lexical scopes");
    require(countDeclarations.size() == 3 && countDeclarations.at("lib/Utility.pipe") == 1 &&
        countDeclarations.at("lib/Extra.pipe") == 1 && countDeclarations.at("app/App.pipe") == 1,
        "same-name local declarations collided across module fragments");
}

void emit(const std::filesystem::path& directory, const Json& result) {
    std::filesystem::create_directories(directory);
    write(directory / "Model.hpp", result.at("header").get<std::string>());
    if (result.contains("types_header"))
        write(directory / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("constants_header"))
        write(directory / "Constants.hpp", result.at("constants_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unsafe inferred implementation path");
            write(directory / name, file.at("content").get<std::string>());
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: var-inference-tests SOURCE_ROOT FRESH_OUTPUT");
        valueContracts();
        loopAndAwaitContracts();
        refusalContracts();
        contextualContracts();
        semanticContracts();

        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh output required");
        auto fixture = read(root / "tests/native-models/VarInference.pipe");
        requireConcreteLocals(checked(fixture));
        emit(output / "objects", compileObjects(fixture, {"App"}, "var_inference"));

        const std::string marker = "public Class App";
        const auto owner = fixture.find(marker);
        require(owner != std::string::npos, "inference runtime root is missing");
        fixture.replace(owner, marker.size(), "public model Class App");
        fixture = "module VarInference;\n" + fixture;
        const std::vector<ModuleSource> modules{{"VarInference", fixture, "VarInference.pipe"}};
        const auto header = compileProjectModules(modules, "VarInference", "var_inference");
        const auto compiled = compileProjectImplementation(modules, "VarInference", "var_inference", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "inferred project emission contracts diverged");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        std::cout << assertions << " var inference compiler assertions passed\n";
        return 0;
    } catch (const Diagnostic& diagnostic) {
        std::cerr << diagnostic.what() << " at byte " << diagnostic.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
