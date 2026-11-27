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

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "cannot read execution segment source fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path);
    output << bytes;
    require(output.good(), "cannot write generated execution segment fixture");
}

void emit(const std::filesystem::path& output, const Json& result) {
    std::filesystem::create_directories(output);
    write(output / "Model.hpp", result.at("header").get<std::string>());
    write(output / "manifest.json", result.at("manifest").dump(2));
    for (const auto& [key, name] : {
        std::pair{"types_header", "Types.hpp"},
        std::pair{"numeric_header", "Numeric.hpp"},
        std::pair{"constants_header", "Constants.hpp"},
        std::pair{"composition_factory_header", "CompositionFactory.hpp"}}) {
        if (result.contains(key))
            write(output / name, result.at(key).get<std::string>());
    }
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unexpected generated artifact path");
            write(output / name, file.at("content").get<std::string>());
        }
    }
}

void generationContract(const Json& result) {
    std::string native = result.at("header").get<std::string>();
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files"))
            native += file.at("content").get<std::string>();
    }
    require(native.find("forInvocation(pl_scope.evaluationStepsPerSegment())") != std::string::npos,
            "normal root did not consume the admitted TaskScope profile");
    require(native.find("forInitialization(pl_options.initializationSteps)") != std::string::npos,
            "invocation profile replaced initialization policy");
    require(native.find("auto& pl_budget = pl_parentBudget ? *pl_parentBudget : pl_ownedBudget") != std::string::npos,
            "async descendants no longer prefer their parent's budget");
    require(native.find("auto& pl_budget = pl_execution.budget") != std::string::npos,
            "ordinary/static helpers no longer borrow root execution");
    if (result.contains("numeric_header")) {
        const auto numeric = result.at("numeric_header").get<std::string>();
        require(numeric.find("evaluationStepsPerSegment") == std::string::npos,
                "native numeric projection references a nonexistent TaskScope");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: execution-segments-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
                "absolute source and fresh output required");
        auto source = read(root / "tests/native-models/ExecutionSegments.pipe");
        const auto objects = compileObjects(source, {"App"}, "execution_segments");
        generationContract(objects);
        emit(output / "objects", objects);
        const std::string marker = "public Class App {";
        const auto owner = source.find(marker);
        require(owner != std::string::npos, "execution segment fixture root missing");
        source.replace(owner, marker.size(), "public model Class App {");
        source = "module Segments;\n" + source;
        source += R"pipe(
public model Class NumericRoot {
    public pure int Add(int value) => value + 1;
}
)pipe";
        const std::vector<ModuleSource> modules{{"Segments", source, "ExecutionSegments.pipe"}};
        const auto header = compileProjectModules(modules, "Segments", "execution_segments");
        const auto compiled = compileProjectImplementation(modules, "Segments", "execution_segments", "sources");
        require(header.at("manifest") == compiled.at("manifest"), "compiled segment source contract diverged");
        generationContract(header);
        generationContract(compiled);
        emit(output / "project", header);
        emit(output / "compiled", compiled);
        source += R"pipe(
public composition Startup {
    scoped input WorkSeed;
    scoped ISegmentWork = SegmentWork;
    export ISegmentWork;
}
)pipe";
        const std::vector<ModuleSource> composition{{"Segments", source, "ExecutionSegments.pipe"}};
        const auto factory = compileProjectCompositionFactory(composition, "Segments", "execution_segments");
        const auto factoryCompiled = compileProjectCompositionFactory(composition, "Segments", "execution_segments", "sources");
        require(factory.at("manifest") == factoryCompiled.at("manifest"), "compiled DI segment contract diverged");
        emit(output / "di_project", factory);
        emit(output / "di_compiled", factoryCompiled);
        std::cout << "execution segment ordinary, static/numeric and DI source artifacts generated\n";
        return 0;
    } catch (const Diagnostic& error) {
        std::cerr << error.what() << " at byte " << error.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
