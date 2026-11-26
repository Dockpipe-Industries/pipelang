// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

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

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    require(output.good(), "cannot write " + path.string());
}

void refused(const std::string& source, const std::string& diagnostic, bool legacy = false) {
    try {
        if (legacy)
            (void)compile(source, "binding_refusal");
        else
            (void)compileObjects(source, {"App"}, "binding_refusal");
    } catch (const Diagnostic& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
            "expected " + diagnostic + ", got " + error.what());
        return;
    }
    throw std::runtime_error("binding admission lost restriction: " + diagnostic);
}

void contracts() {
    refused(R"pipe(
public Class App {
    private int value;
    public App(int value) {}
}
)pipe", "parameters must not shadow model fields", true);

    const auto capturedInput = compileObjects(R"pipe(
public Class Item {
    private int initialized = argument;
    public Item(int argument) {}
}
public Class App {}
)pipe", {"App"}, "binding_constructor_input");
    require(capturedInput.at("ok") == true,
        "declaration initializer lost its existing constructor input");

    refused(R"pipe(
public Class App {
    private int initialized = missing;
    public App(int argument) {}
}
)pipe", "unknown value 'missing'");

    refused(R"pipe(
public Class Base {
    public int value;
    public Base(int value) { this.value = value; }
}
public Class Child : Base {
    public Child(int value) : base(this.value) {}
}
public Class App {}
)pipe", "base arguments cannot access the instance");

    refused(R"pipe(
public Class Base { private int value; }
public Class Child : Base {
    public pure int Read(int value) => this.value;
}
public Class App {}
)pipe", "private object field");

    refused(R"pipe(
public Class App {
    private int value;
    public pure int Change(int value) {
        this.value = value;
        return value;
    }
}
)pipe", "pure methods cannot mutate model fields");

    refused(R"pipe(
public Class App {
    private int value;
    private void Worker(int value) { this.value = value; }
    public void Run(List<int> inputs) {
        parallel foreach (int item in inputs) { this.Worker(item); }
    }
}
)pipe", "parallel helper cannot mutate ordinary model state");

    refused(R"pipe(
public Class Worker {
    private int value;
    private pure int Read(int value) => this.value;
    public void Run(List<int> inputs) {
        parallel foreach (int item in inputs) { int result = Read(item); }
    }
}
public Class App {}
)pipe", "cannot access state");

    refused(R"pipe(
public Class App {
    private Atomic<int> total;
    public void Change(Atomic<int> total) { this.total = total; }
}
)pipe", "atomic field identity is fixed");

    refused(R"pipe(
public Interface Host { pure int Read(); }
public Class App {
    private Host host;
    public App(Host host) { this.host = host; }
    public void Change() { this.host = this.host; }
}
)pipe", "service identity is fixed");

    refused("public Class App { public pure int Read(int value, int value) => value; }", "duplicate parameter");
    refused("public Class App { public void Run(int value) { int value = 3; } }", "unique unshadowed");
    refused("public Class App { public pure App Read() => this; }", "unknown value or service type");

    const std::string privateParameter = R"pipe(
public Class Base { private int value; }
public Class Child : Base {
    public pure int Read(int value) => value;
}
public Class App {}
)pipe";
    require(compileObjects(privateParameter, {"App"}, "private_parameter").at("ok") == true,
        "a lexical parameter was mistaken for an inaccessible inherited field");
}

void emit(const std::filesystem::path& output, const Json& result) {
    std::filesystem::create_directories(output);
    write(output / "Model.hpp", result.at("header").get<std::string>());
    write(output / "manifest.json", result.at("manifest").dump(2));
    if (result.contains("types_header"))
        write(output / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("constants_header"))
        write(output / "Constants.hpp", result.at("constants_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unexpected generated path");
            write(output / name, file.at("content").get<std::string>());
        }
    }
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: member-bindings-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh proof directory required");
        contracts();
        auto source = read(root / "tests/native-models/MemberBindings.pipe");
        emit(output / "objects", compileObjects(source, {"App"}, "member_bindings"));
        const std::string marker = "public Class App {";
        const auto owner = source.find(marker);
        require(owner != std::string::npos, "binding fixture root missing");
        source.replace(owner, marker.size(), "public model Class App {");
        source += R"pipe(
public model Class FacetProbe {
    private App app;
    public FacetProbe(BindingHost host, int value) {
        app = new App(host, value, 43);
    }
    public pure int Initial() => app.initialized;
    public pure int Parameter(int value) => app.Parameter(value);
    public pure int GenericCall(int value) => app.GenericCall(value);
    public int Workers(List<int> values) => app.Workers(values);
    public async Task<Result<int,string>> Forward(int value) {
        Result<int,string> result = await app.Forward(value);
        return result;
    }
}
)pipe";
        source = "module Binding;\n" + source;
        const std::vector<ModuleSource> modules{{"Binding", source, "MemberBindings.pipe"}};
        const auto header = compileProjectModules(modules, "Binding", "member_bindings");
        const auto compiled = compileProjectImplementation(modules, "Binding", "member_bindings", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled binding changed the public contract");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        try {
            (void)compileProjectModules({{"Binding", "module Binding; public model Class App { public pure App Self() => this; }"}},
                "Binding", "escaped_owner");
            throw std::runtime_error("native owner escaped as a first-class this reference");
        } catch (const Diagnostic& error) {
            require(std::string(error.what()).find("native owner facet") != std::string::npos,
                "bare native-root this lost its explicit facet diagnostic");
        }
        std::cout << "member bindings: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
