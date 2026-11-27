// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/model-compiler/AsyncFinallyLowering.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
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

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    require(output.good(), "cannot write " + path.string());
}

void regionBookkeeping() {
    AsyncFinallyLowering lowering;
    require(lowering.returning() == nullptr, "return outside a protected region gained a cleanup target");
    lowering.enter(1);
    lowering.loopTransfer(1, true);
    const auto continued = lowering.leave();
    require(continued.continues && !continued.breaks && !continued.returns,
        "continue-only region gained a pending return");

    lowering.enter(1);
    lowering.loopTransfer(1, false);
    const auto broken = lowering.leave();
    require(broken.breaks && !broken.continues && !broken.returns,
        "break-only region gained a pending return");

    lowering.enter(0);
    lowering.enter(0);
    require(lowering.returning() != nullptr, "inner return lost its region");
    const auto inner = lowering.leave();
    require(inner.returns, "explicit inner return was not recorded");
    require(lowering.returning() != nullptr, "outward return lost its enclosing region");
    require(lowering.leave().returns, "outward return did not mark the enclosing region");
}

struct Case {
    std::string name;
    std::string body;
    std::size_t regions;
    std::size_t returningRegions;
};

const std::vector<Case> cases{
    {"fallthrough", R"pipe(
        try {
            int value = 1;
        } finally {
            Result<int,string> cleanup = await Pause();
        }
        return Result<int,string>.Success(1);
    )pipe", 1, 0},
    {"throw-only", R"pipe(
        try {
            throw Error("primary");
        } finally {
            Result<int,string> cleanup = await Pause();
        }
    )pipe", 1, 0},
    {"conditional-return", R"pipe(
        try {
            if (choose)
                return Result<int,string>.Success(3);
            throw Error("branch");
        } finally {
            Result<int,string> cleanup = await Pause();
        }
    )pipe", 1, 1},
    {"conditional-throw", R"pipe(
        try {
            if (choose)
                throw Error("branch");
            int value = 1;
        } finally {
            Result<int,string> cleanup = await Pause();
        }
        return Result<int,string>.Success(4);
    )pipe", 1, 0},
    {"continue", R"pipe(
        int index = 0;
        while (index < 2) {
            try {
                index = index + 1;
                continue;
            } finally {
                Result<int,string> cleanup = await Pause();
            }
        }
        return Result<int,string>.Success(index);
    )pipe", 1, 0},
    {"break", R"pipe(
        while (true) {
            try {
                break;
            } finally {
                Result<int,string> cleanup = await Pause();
            }
        }
        return Result<int,string>.Success(2);
    )pipe", 1, 0},
    {"nested-explicit", R"pipe(
        try {
            try {
                return Result<int,string>.Success(7);
            } finally {
                Result<int,string> inner = await Pause();
            }
        } finally {
            Result<int,string> outer = await Pause();
        }
    )pipe", 2, 2},
    {"nested-propagation", R"pipe(
        try {
            try {
                int value = try Result<int,string>.Failure("propagated");
                return Result<int,string>.Success(value);
            } finally {
                Result<int,string> inner = await Pause();
            }
        } finally {
            Result<int,string> outer = await Pause();
        }
    )pipe", 2, 2},
    {"mixed-nesting", R"pipe(
        try {
            try {
                int value = 1;
            } finally {
                Result<int,string> inner = await Pause();
            }
            return Result<int,string>.Success(9);
        } finally {
            Result<int,string> outer = await Pause();
        }
    )pipe", 2, 1}
};

std::string source(const Case& test, bool project) {
    std::string result;
    if (project)
        result = "module Scaling;\n";
    result += project ? "public model Class App {\n" : "public Class App {\n";
    result += R"pipe(
    private async Task<Result<int,string>> Pause() {
        return Result<int,string>.Success(0);
    }
    public async Task<Result<int,string>> Run(bool choose) {
)pipe";
    result += test.body;
    result += "\n    }\n}\n";
    return result;
}

struct RegionOutput {
    std::size_t declarations = 0;
    std::size_t storageFreeDeclarations = 0;
    std::size_t returningCalls = 0;
    std::size_t returnArms = 0;
};

void inspect(const Json& response, const Case& test, std::size_t copies) {
    std::string generated = response.at("header").get<std::string>();
    if (response.contains("implementation")) {
        for (const auto& file : response.at("implementation").at("files"))
            generated += file.at("content").get<std::string>();
    }
    std::map<std::string, RegionOutput> regions;
    std::istringstream lines(generated);
    std::string line;
    while (std::getline(lines, line)) {
        const auto start = line.find("pl_exit");
        if (start == std::string::npos)
            continue;
        const auto end = line.find_first_not_of("0123456789", start + std::string("pl_exit").size());
        const auto identity = line.substr(start, end - start);
        auto& region = regions[identity];
        if (line.find("AsyncFinallyExit<") != std::string::npos) {
            ++region.declarations;
            if (line.find(", false> " + identity + ";") != std::string::npos)
                ++region.storageFreeDeclarations;
        }
        if (line.find(identity + ".returning(") != std::string::npos)
            ++region.returningCalls;
        if (line.find(identity + ".isReturn()") != std::string::npos)
            ++region.returnArms;
    }
    require(regions.size() == test.regions * copies, test.name + ": protected region count changed");
    std::size_t returning = 0;
    for (const auto& [identity, region] : regions) {
        require(region.declarations == 1, test.name + ": duplicate or missing exit storage for " + identity);
        if (region.returningCalls == 0) {
            require(region.storageFreeDeclarations == 1,
                test.name + ": nonreturning region retained result storage for " + identity);
            require(region.returnArms == 0, test.name + ": impossible return/commit/co_return arm retained");
        } else {
            require(region.storageFreeDeclarations == 0,
                test.name + ": returning region lost result storage for " + identity);
            require(region.returnArms == 1, test.name + ": explicit/propagated pending return lost its arm");
            ++returning;
        }
    }
    require(returning == test.returningRegions * copies,
        test.name + ": nested explicit/Result return routing changed");
}

void emit(const std::filesystem::path& directory, const Json& response) {
    std::filesystem::create_directories(directory);
    write(directory / "Model.hpp", response.at("header").get<std::string>());
    write(directory / "manifest.json", response.at("manifest").dump(2));
    if (response.contains("types_header"))
        write(directory / "Types.hpp", response.at("types_header").get<std::string>());
    if (response.contains("implementation")) {
        for (const auto& file : response.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unsafe compiler artifact name");
            write(directory / name, file.at("content").get<std::string>());
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: async-finally-scaling-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && std::filesystem::is_directory(root) && output.is_absolute() &&
            !std::filesystem::exists(output), "absolute source root and fresh output required");
        regionBookkeeping();
        for (const auto& test : cases) {
            const auto objects = compileObjects(source(test, false), {"App"}, "scaling");
            inspect(objects, test, 1);
            emit(output / test.name / "objects", objects);

            const std::vector<ModuleSource> modules{{"Scaling", source(test, true), test.name + ".pipe"}};
            const auto project = compileProjectModules(modules, "Scaling", "scaling");
            const auto compiled = compileProjectImplementation(modules, "Scaling", "scaling", "sources");
            require(project.at("manifest") == compiled.at("manifest"), test.name + ": compiled contract diverged");
            inspect(project, test, 2);
            inspect(compiled, test, 2);
            emit(output / test.name / "project", project);
            emit(output / test.name / "compiled", compiled);
        }
        std::cout << "async finally scaling: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
