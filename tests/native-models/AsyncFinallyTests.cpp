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

void require(bool value, const std::string& message) {
    ++assertions;
    if (!value)
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

void refused(const std::string& source, const std::string& diagnostic, bool legacy = false) {
    try {
        if (legacy)
            (void)compile(source, "finally_refusal");
        else
            (void)compileObjects(source, {"App"}, "finally_refusal");
    } catch (const Diagnostic& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
            "expected " + diagnostic + ", got " + error.what());
        return;
    }
    throw std::runtime_error("invalid source admitted: " + diagnostic);
}

void contracts() {
    const std::string child = "private async Task<Result<int,string>> Child() { return Result<int,string>.Success(1); }";
    const std::string finally = "try {} finally { Result<int,string> child = await Child(); } return Result<int,string>.Success(1);";
    const std::string ordinary = "public Class App { " + child + " public async Task<Result<int,string>> Run() { " + finally + " } }";
    refused(ordinary, "finally cannot suspend", true);
    require(compileObjects(ordinary, {"App"}, "finally_admitted").at("ok") == true,
        "ordinary directly awaited child cleanup was rejected");
    refused("public Class App { " + child + " public int Run() { try {} finally { Result<int,string> child = await Child(); } return 1; } }",
        "async method");
    refused("public Class App { public async Task<Result<int,string>> Run() { try {} finally { return Result<int,string>.Success(1); } } }",
        "finally cannot return");
    refused("public Class App { public async Task<Result<int,string>> Run() { while (true) { try {} finally { break; } } return Result<int,string>.Success(1); } }",
        "same cleanup scope");
    refused("public Class App { public async Task<Result<int,string>> Run() { try {} finally { int value = try Result<int,string>.Failure(\"bad\"); } return Result<int,string>.Success(1); } }",
        "finally cannot propagate");
    refused("public Class App { private Lock gate; " + child + " public async Task<Result<int,string>> Run() { lock (gate) { Result<int,string> child = await Child(); } return Result<int,string>.Success(1); } }",
        "lock bodies cannot suspend");
    refused("public Class App { private SemaphoreSlim gate; public async Task<Result<int,string>> Run() { try {} finally { gate.Wait(); } return Result<int,string>.Success(1); } }",
        "blocking SemaphoreSlim.Wait");
    refused("public Class App { private void Worker(List<int> values) { parallel foreach (int value in values) {} } public async Task<Result<int,string>> Run(List<int> values) { try {} finally { Worker(values); } return Result<int,string>.Success(1); } }",
        "finally cannot start parallel");
    refused("public Class App { " + child + " public void Run(List<int> values) { parallel foreach (int value in values) { Result<int,string> child = await Child(); } } }",
        "parallel workers cannot await");
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: async-finally-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh output required");
        contracts();
        auto source = read(root / "tests/native-models/AsyncFinally.pipe");
        emit(output / "objects", compileObjects(source, {"App"}, "async_finally"));
        const std::string marker = "public Class App {";
        const auto owner = source.find(marker);
        require(owner != std::string::npos, "missing source fixture root");
        source.replace(owner, marker.size(), "public model Class App {");
        source = "module Cleanup;\n" + source;
        source += R"pipe(
public model Class FacetProbe {
    private App owner;
    public FacetProbe(CleanupHost host) { owner = new App(host); }
    public async Task<Result<int,string>> Run(CancellationToken token) {
        Result<int,string> result = await owner.CleanupHelpers(token);
        return result;
    }
}
)pipe";
        const std::vector<ModuleSource> modules{{"Cleanup", source, "AsyncFinally.pipe"}};
        const auto header = compileProjectModules(modules, "Cleanup", "async_finally");
        const auto compiled = compileProjectImplementation(modules, "Cleanup", "async_finally", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled cleanup contract diverged");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        const auto probe = read(root / "tests/native-models/AsyncFinallyProbe.pipe");
        emit(output / "probe", compileProjectModules({{"AsyncCleanupProbe", probe, "AsyncFinallyProbe.pipe"}},
            "AsyncCleanupProbe", "async_finally_probe"));
        std::cout << "async finally: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const Diagnostic& error) {
        std::cerr << error.what() << " at byte " << error.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
