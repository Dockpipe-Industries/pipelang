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
    std::ifstream stream(path);
    require(stream.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream stream(path);
    stream << value;
    require(stream.good(), "cannot write " + path.string());
}

void emit(const std::filesystem::path& output, const Json& result) {
    std::filesystem::create_directories(output);
    write(output / "Model.hpp", result.at("header").get<std::string>());
    write(output / "manifest.json", result.at("manifest").dump(2));
    if (result.contains("types_header"))
        write(output / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("numeric_header"))
        write(output / "Numeric.hpp", result.at("numeric_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "invalid compiler artifact name");
            write(output / name, file.at("content").get<std::string>());
        }
    }
}

void refused(const std::string& source, const std::string& expected, bool legacy = false) {
    try {
        if (legacy)
            (void)compile(source, "bounded_refusal");
        else
            (void)compileObjects(source, {"App"}, "bounded_refusal");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(expected) != std::string::npos,
            "expected " + expected + ", got " + diagnostic.what());
        return;
    }
    throw std::runtime_error("admitted invalid source: " + expected);
}

bool exportsMethod(const Json& result, const std::string& method) {
    const auto& manifest = result.at("manifest");
    if (!manifest.contains("nativeNumericExports"))
        return false;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("method") == method)
            return true;
    }
    return false;
}

void contracts() {
    refused("public Class App { public pure int Loop() => Loop(); }", "recursive local call", true);
    refused("public Class App { public async Task<Result<int,string>> Loop() { Result<int,string> value = await Loop(); return value; } }",
        "recursive local call", true);
    refused("public Class App { private async Task<Result<int,string>> Loop() { Result<int,string> value = Loop(); return value; } }",
        "directly awaited");
    refused("public Class App { private int state; private int Worker(int value) { state = value; return Worker(value); } public void Run(List<int> values) { parallel foreach (int value in values) { int result = Worker(value); } } }",
        "parallel helper cannot mutate");
    refused("public Class App { private void Child(List<int> values) { parallel foreach (int value in values) { } } public void Run(List<int> values) { try { } finally { Child(values); } } }",
        "finally cannot start parallel");

    const auto cyclic = compileProjectModules({{"Policy", R"pipe(module Policy;
public model Class Policy { public pure bool Loop() => Loop(); }
)pipe"}}, "Policy", "numeric_cycle");
    require(cyclic.at("ok") == true && !exportsMethod(cyclic, "Loop"),
        "cyclic source method escaped through an unguarded numeric export");

    for (const int count : {32, 33}) {
        std::string source = "module Policy; public model Class Policy {";
        for (int index = 0; index < count; ++index) {
            source += " public pure bool Call" + std::to_string(index) + "() => ";
            source += index + 1 == count ? "true;" : "Call" + std::to_string(index + 1) + "();";
        }
        source += " }";
        const auto result = compileProjectModules({{"Policy", source}}, "Policy", "numeric_depth");
        require(result.at("ok") == true, "deep ordinary source method was rejected");
        require(exportsMethod(result, "Call0") == (count == 32), "numeric source depth proof differs from 32");
    }
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: bounded-calls-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source and fresh proof paths required");
        contracts();
        auto source = read(root / "tests/native-models/BoundedCalls.pipe");
        const auto objects = compileObjects(source, {"App"}, "bounded_calls");
        emit(output / "objects", objects);

        const std::string marker = "public Class App {";
        const auto owner = source.find(marker);
        require(owner != std::string::npos, "fixture has no root");
        source.replace(owner, marker.size(), "public model Class App { public int Qualified(int depth) => QualifiedPolicy.Recur(depth);");
        source += R"pipe(
public model Class QualifiedPolicy {
    public pure int Recur(int depth) {
        if (depth == 0)
            return 23;
        return Recur(depth - 1);
    }
}
)pipe";
        source = "module Bounded;\n" + source;
        const std::vector<ModuleSource> modules{{"Bounded", source, "BoundedCalls.pipe"}};
        const auto header = compileProjectModules(modules, "Bounded", "bounded_calls");
        const auto compiled = compileProjectImplementation(modules, "Bounded", "bounded_calls", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled emission changed source contracts");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        std::cout << "bounded source calls: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
