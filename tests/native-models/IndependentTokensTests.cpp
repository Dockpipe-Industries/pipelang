// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

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

void emit(const std::filesystem::path& output, const Json& result) {
    std::filesystem::create_directories(output);
    write(output / "Model.hpp", result.at("header").get<std::string>());
    write(output / "manifest.json", result.at("manifest").dump(2));
    for (const auto& [key, name] : {
        std::pair{"types_header", "Types.hpp"},
        std::pair{"constants_header", "Constants.hpp"},
        std::pair{"composition_factory_header", "CompositionFactory.hpp"}}) {
        if (result.contains(key))
            write(output / name, result.at(key).get<std::string>());
    }
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unexpected compiler artifact path");
            write(output / name, file.at("content").get<std::string>());
        }
    }
}

void compatibility() {
    const std::string source = R"pipe(
public Interface Host { Task<Result<int,string>> Work(CancellationToken token); }
public Class App {
    private Host host;
    public App(Host input) { host = input; }
    private async Task<Result<int,string>> Child(CancellationToken first, CancellationToken second) {
        Result<int,string> value = await host.Work(first);
        return value;
    }
    public async Task<Result<int,string>> Run(CancellationToken first, CancellationToken second) {
        Result<int,string> value = await Child(first, second);
        return value;
    }
}
)pipe";
    const auto legacy = compile(source, "tokens_legacy").at("header").get<std::string>();
    require(legacy.find("pl_taskCancellation = pipelang::model::CancellationToken::combine(pl_taskCancellation, first)") != std::string::npos,
        "legacy root cancellation aggregation changed");
    require(legacy.find("first = pipelang::model::CancellationToken::combine(first, pl_taskCancellation)") != std::string::npos,
        "legacy token parameter rewriting changed");
    const auto modern = compileObjects(source, {"App"}, "tokens_modern").at("header").get<std::string>();
    require(modern.find("pl_taskCancellation = pipelang::model::CancellationToken::combine") == std::string::npos,
        "modern source parameters still alter implicit task cancellation");
    require(modern.find("first = pipelang::model::CancellationToken::combine") == std::string::npos,
        "modern source token identity was rewritten");
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: independent-tokens-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh output required");
        compatibility();
        auto source = read(root / "tests/native-models/IndependentTokens.pipe");
        emit(output / "objects", compileObjects(source, {"App"}, "independent_tokens"));
        const std::string marker = "public Class App {";
        const auto owner = source.find(marker);
        require(owner != std::string::npos, "token fixture model root missing");
        source.replace(owner, marker.size(), "public model Class App {");
        source = "module Tokens;\n" + source;
        const std::vector<ModuleSource> modules{{"Tokens", source, "IndependentTokens.pipe"}};
        const auto header = compileProjectModules(modules, "Tokens", "independent_tokens");
        const auto compiled = compileProjectImplementation(modules, "Tokens", "independent_tokens", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled token contract diverged");
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        source += R"pipe(
public composition Startup {
    singleton input TokenHost;
    scoped input ScopeStamp;
    scoped ITokenPath = TokenWorker;
    export ITokenPath;
}
)pipe";
        const std::vector<ModuleSource> composition{{"Tokens", source, "IndependentTokens.pipe"}};
        const auto factory = compileProjectCompositionFactory(composition, "Tokens", "independent_tokens");
        const auto factoryCompiled = compileProjectCompositionFactory(composition, "Tokens", "independent_tokens", "sources");
        const auto facade = factory.at("composition_factory_header").get<std::string>();
        require(facade.find("pl_taskCancellation = pipelang::model::CancellationToken::combine") == std::string::npos,
            "SourceMethods contaminated implicit cancellation");
        require(facade.find("pl_cancellation = pipelang::model::CancellationToken::combine(pl_cancellation, pl_argument_") == std::string::npos,
            "DI admission still combines authored tokens with its boundary token");
        require(factory.at("manifest") == factoryCompiled.at("manifest"), "compiled DI token contract diverged");
        emit(output / "di_project", factory);
        emit(output / "di_compiled", factoryCompiled);
        std::cout << "independent tokens: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const Diagnostic& error) {
        std::cerr << error.what() << " at byte " << error.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
