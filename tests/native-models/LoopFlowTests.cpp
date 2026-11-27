// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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

Json admit(const std::string& body, bool legacy = false) {
    const auto source = "public Class App { public int Run(bool choose) { " + body + " } }";
    if (legacy)
        return compile(source, "loop_flow_contract");
    return compileObjects(source, {"App"}, "loop_flow_contract");
}

void accepted(const std::string& body, const std::string& purpose) {
    require(admit(body).at("ok") == true, purpose);
}

void refused(const std::string& body, const std::string& diagnostic, bool legacy = false) {
    try {
        (void)admit(body, legacy);
    } catch (const Diagnostic& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
            "expected " + diagnostic + ", got " + error.what());
        return;
    }
    throw std::runtime_error("invalid loop return flow admitted: " + diagnostic);
}

void contracts() {
    accepted("while (true) { return 1; }", "literal-true return refused");
    accepted("while (true) {}", "empty infinite while incorrectly falls through");
    accepted("while (true) { if (choose) return 1; continue; }", "continue invented a loop exit");
    accepted("for (;;) {}", "omitted for condition lost its literal-true semantics");
    accepted("for (int index = 0; true; index = index + 1) { if (choose) return index; }",
        "for iteration invented fallthrough");
    accepted("while (true) { while (choose) { break; } }", "inner break escaped to outer loop");
    accepted("for (;;) { while (choose) { break; } }", "inner break escaped to outer for");
    accepted("while (true) { while (true) { break; } return 2; }", "inner break lost its own exit");
    accepted("while (true) { try { break; } finally { throw Error(\"stop\"); } }",
        "throwing finally did not override break");
    accepted("try { while (true) { if (choose) return 1; } } finally { int cleanup = 0; }",
        "finally invented fallthrough from a non-fallthrough body");
    accepted("try { while (true) {} } catch (Error error) { return 3; }",
        "returning catch added a normal loop exit");
    refused("while (true) { break; }", "method must return on every path");
    refused("while (true) { if (choose) break; return 1; }", "method must return on every path");
    refused("for (;;) { if (choose) break; }", "method must return on every path");
    refused("while (choose) { return 1; }", "method must return on every path");
    refused("while (false) { return 1; }", "method must return on every path");
    refused("bool looping = true; while (looping) { return 1; }", "method must return on every path");
    refused("while (!false) { return 1; }", "method must return on every path");
    refused("List<int> values = List<int>.Empty(); foreach (int item in values) return item;",
        "method must return on every path");
    refused("while (true) { if (false) break; }", "method must return on every path");
    refused("while (true) { try { break; } finally { int cleanup = 0; } }",
        "method must return on every path");
    refused("while (true) { try { continue; } catch (Error error) { break; } }",
        "method must return on every path");
    refused("while (true) { continue; } return 1;", "unreachable statement");
    refused("while (true) { return 1; break; }", "unreachable statement");
    refused("while (true) { try {} finally { break; } }", "same cleanup scope");
    refused("while (true) { return 1; }", "method must return on every path", true);

    const std::string async = R"pipe(
public Interface Reader { Task<Result<int,string>> Read(CancellationToken token); }
public Class App {
    private Reader reader;
    public App(Reader reader) { this.reader = reader; }
    public async Task<Result<int,string>> Run(CancellationToken token) {
        while (true) {
            Result<int,string> observed = await reader.Read(token);
            if (!observed.IsSuccess)
                return observed;
            if (observed.Value > 0)
                return observed;
        }
    }
}
)pipe";
    require(compileObjects(async, {"App"}, "loop_flow_async").at("ok") == true,
        "awaited observation loop required a dummy return");
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path);
    output << bytes;
    require(output.good(), "cannot write " + path.string());
}

void emit(const std::filesystem::path& directory, const Json& result) {
    std::filesystem::create_directories(directory);
    write(directory / "Model.hpp", result.at("header").get<std::string>());
    write(directory / "manifest.json", result.at("manifest").dump(2));
    if (result.contains("types_header"))
        write(directory / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("constants_header"))
        write(directory / "Constants.hpp", result.at("constants_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "invalid generated output path");
            write(directory / name, file.at("content").get<std::string>());
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: loop-flow-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path sourceRoot(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(sourceRoot.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh output required");
        contracts();
        auto fixture = read(sourceRoot / "tests/native-models/LoopFlow.pipe");
        emit(output / "objects", compileObjects(fixture, {"App"}, "loop_flow"));
        const std::string marker = "public Class App";
        const auto owner = fixture.find(marker);
        require(owner != std::string::npos, "runtime model root not found");
        fixture.replace(owner, marker.size(), "public model Class App");
        fixture = "module LoopFlow;\n" + fixture;
        const std::vector<ModuleSource> modules{{"LoopFlow", fixture, "LoopFlow.pipe"}};
        const auto header = compileProjectModules(modules, "LoopFlow", "loop_flow");
        const auto compiled = compileProjectImplementation(modules, "LoopFlow", "loop_flow", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled loop contract diverged");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        std::cout << "literal-true loop flow: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const Diagnostic& error) {
        std::cerr << error.what() << " at byte " << error.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
