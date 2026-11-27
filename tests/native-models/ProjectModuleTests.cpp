// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ProjectModuleTests.hpp"
#include "BuildTimestamp.h"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include "../../src/native/sdk-tools/bindings.hpp"
#include <algorithm>
#include <fstream>

namespace {
using namespace pipelang::tools;

Json compileProject(const fs::path& prefix, const Json& request) {
    const auto response = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                  request.dump(), std::chrono::seconds(45), 4U << 20);
    require(response.exit == 0 && response.diagnostic.empty(),
            "project fragment compiler transport failed: " + response.diagnostic);
    return parseJson(response.output, 4U << 20);
}

void requireRefusal(const std::function<void()>& operation, const std::string& message) {
    try {
        operation();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(message) != std::string::npos,
                "project fragment refusal omitted " + message + ": " + error.what());
        return;
    }
    throw std::runtime_error("project fragments unexpectedly accepted " + message);
}

void requireDiagnostic(const Json& response, const std::string& path, std::size_t offset) {
    require(!response.at("ok").get<bool>(), "invalid fragment unexpectedly compiled");
    const auto& diagnostic = response.at("diagnostics").at(0);
    require(diagnostic.at("path") == path && diagnostic.at("offset") == offset,
            "fragment diagnostic changed source origin or offset: " + response.dump());
}

void writeFragments(const fs::path& root) {
    atomicWrite(root / "src/entry/Entry.pipe", R"pipe(module Gameplay;
import Data;
public Record Start {
    public Pair pair;
    public CounterValue counter;
}
)pipe");
    atomicWrite(root / "src/domain/CounterValue.pipe", R"pipe(module Gameplay;
import Helpers;
public Record CounterValue { public Extra extra; }
)pipe");
    atomicWrite(root / "src/domain/Imports.pipe", "module Gameplay; import Data;\n");
    atomicWrite(root / "src/data/Pair.pipe", "module Data; public Record Pair { public int value; }\n");
    atomicWrite(root / "src/helpers/Extra.pipe", "module Helpers; public Record Extra { public int value; }\n");
}

void admissionAndOrigins(const fs::path& prefix, const fs::path& root) {
    const auto project = SourceProject::load(root, "src", "entry/Entry.pipe");
    const auto request = project.request("module_fixture");
    require(project.dependencies().size() == 5 && request.at("modules").size() == 5,
            "same-module discovery lost a contributing file");
    require(project.receipt().at("closure").size() == 3 && project.receipt().at("closure_sources").size() == 5,
            "project fragments were counted as separate module identities");
    for (const auto& fragment : request.at("modules")) {
        const auto path = fragment.at("path").get<std::string>();
        require(fragment.at("source") == readFile(root / "src" / path, 65536),
                "project fragment transport rewrote source bytes");
    }
    const auto accepted = compileProject(prefix, request);
    require(accepted.at("ok") == true, "same-module types did not compile: " + accepted.dump());
    auto reordered = request;
    std::reverse(reordered["modules"].begin(), reordered["modules"].end());
    require(compileProject(prefix, reordered) == accepted,
            "module fragment input order changed deterministic compiler output");

    auto lexical = request;
    const std::string invalidImport = "module Gameplay; public Record Start { public Extra value; }";
    for (auto& fragment : lexical["modules"]) {
        if (fragment.at("path") == "entry/Entry.pipe")
            fragment["source"] = invalidImport;
    }
    requireDiagnostic(compileProject(prefix, lexical), "entry/Entry.pipe", invalidImport.find("public Extra"));

    auto duplicate = request;
    const std::string repeated = "module Gameplay; public Record CounterValue {}";
    for (auto& fragment : duplicate["modules"]) {
        if (fragment.at("path") == "entry/Entry.pipe")
            fragment["source"] = repeated;
    }
    const auto duplicateResult = compileProject(prefix, duplicate);
    requireDiagnostic(duplicateResult, "entry/Entry.pipe", repeated.find("Record"));
    const auto duplicateMessage = duplicateResult.at("diagnostics").at(0).at("message").get<std::string>();
    require(duplicateMessage.find("domain/CounterValue.pipe") != std::string::npos &&
            duplicateMessage.find("entry/Entry.pipe") != std::string::npos,
            "duplicate declaration diagnostic omitted a contributing file");

    auto checkerFailure = request;
    const std::string invalidValue = "module Gameplay; public Record Start { public int value = true; }";
    for (auto& fragment : checkerFailure["modules"]) {
        if (fragment.at("path") == "entry/Entry.pipe")
            fragment["source"] = invalidValue;
    }
    requireDiagnostic(compileProject(prefix, checkerFailure), "entry/Entry.pipe", invalidValue.find("true"));

    auto normalizerFailure = request;
    const std::string invalidGeneric = "module Gameplay; public Record Start<T> { private int value; }";
    for (auto& fragment : normalizerFailure["modules"]) {
        if (fragment.at("path") == "entry/Entry.pipe")
            fragment["source"] = invalidGeneric;
    }
    requireDiagnostic(compileProject(prefix, normalizerFailure), "entry/Entry.pipe", invalidGeneric.find("private"));

    const std::string cycleA = "module Cycle; public Class A { public const int Value = B.Value; }";
    const std::string cycleB = "module Cycle; public Class B { public const int Value = A.Value; }";
    Json constantCycle = {{"protocol", "pipelang.native-model-project.v1"}, {"entry", "Cycle"},
                         {"modules", {{{"name", "Cycle"}, {"path", "cycle/A.pipe"}, {"source", cycleA}},
                                      {{"name", "Cycle"}, {"path", "cycle/B.pipe"}, {"source", cycleB}}}}};
    requireDiagnostic(compileProject(prefix, constantCycle), "cycle/A.pipe", cycleA.find("public const"));

    for (const auto& spelling : {"glob[part]", "glob*part", "glob?part", "$<Expression>"}) {
        const bool expression = std::string(spelling).starts_with("$<");
        requireRefusal([&] { SourceProject::load(root / spelling, "src", "Entry.pipe"); },
                       expression ? "generator expressions" : "glob metacharacters");
        requireRefusal([&] { SourceProject::load(root, spelling, "Entry.pipe"); },
                       expression ? "generator expressions" : "glob metacharacters");
    }
#ifndef _WIN32
    const auto expressionSource = root / "src/$<Expression>.pipe";
    atomicWrite(expressionSource, "module Dependency; public Record Data {}\n");
    requireRefusal([&] { SourceProject::load(root, "src", "entry/Entry.pipe"); }, "generator expressions");
    fs::remove(expressionSource);
#endif
    const auto added = root / "src/domain/Added.pipe";
    atomicWrite(added, "module Gameplay; public Record Added {}\n");
    requireRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    const auto withAdded = SourceProject::load(root, "src", "entry/Entry.pipe");
    fs::remove(added);
    requireRefusal([&] { withAdded.verifyUnchanged(); }, "changed during compilation");
    const auto counter = root / "src/domain/CounterValue.pipe";
    const auto original = readFile(counter, 65536);
    atomicWrite(counter, "module Gameplay; import Data; public Record CounterValue {}\n");
    requireRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    atomicWrite(counter, original);
    atomicWrite(root / "src/Gameplay.pipe", "public Record Implicit {}\n");
    requireRefusal([&] { SourceProject::load(root, "src", "entry/Entry.pipe"); }, "duplicate project module");
    fs::remove(root / "src/Gameplay.pipe");
    project.verifyUnchanged();
}

void boundedProtocols(const fs::path& prefix) {
    Json project = {{"protocol", "pipelang.native-model-project.v1"}, {"entry", "Shared"},
                    {"namespace", "module_bounds"}, {"modules", Json::array()}};
    for (int index = 0; index < 4096; ++index) {
        project["modules"].push_back({{"name", "Shared"}, {"source", "module Shared;"},
                                    {"path", "parts/" + std::string(180, 'p') + "/P" + std::to_string(index) + ".pipe"}});
    }
    require(compileProject(prefix, project).at("ok") == true,
            "project incorrectly limited physical fragments to the logical module bound");
    project["modules"].push_back({{"name", "Shared"}, {"source", "module Shared;"}, {"path", "parts/Excess.pipe"}});
    const auto excessive = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()}, project.dump(),
                                   std::chrono::seconds(45), 4U << 20);
    require(excessive.exit != 0, "project admitted more than 4096 source fragments");
    project["modules"] = Json::array();
    project["entry"] = "M0";
    for (int index = 0; index < 129; ++index) {
        const auto module = "M" + std::to_string(index);
        const auto dependency = index == 128 ? std::string{} : " import M" + std::to_string(index + 1) + ";";
        project["modules"].push_back({{"name", module}, {"source", "module " + module + ";" + dependency},
                                    {"path", module + ".pipe"}});
    }
    require(compileProject(prefix, project).at("ok") == false, "project admitted more than 128 module identities");
    project["entry"] = "Shared";
    const auto padded = std::string("module Shared;") + std::string(32768 - 14, ' ');
    require(padded.size() == 32768, "fragment byte-budget fixture size drifted");
    project["modules"] = Json::array({{{"name", "Shared"}, {"source", padded}, {"path", "A.pipe"}},
                                      {{"name", "Shared"}, {"source", padded}, {"path", "B.pipe"}}});
    require(compileProject(prefix, project).at("ok") == true, "project rejected exact 64KiB aggregate source");
    project["modules"][1]["source"] = padded + ' ';
    require(compileProject(prefix, project).at("ok") == true, "project retained the obsolete 64KiB source closure ceiling");

    Json legacy = {{"protocol", "pipelang.native-model.v2"}, {"entry", "Shared"},
                   {"modules", {{{"name", "Shared"}, {"source", "module Shared; public Record A {}"}},
                                {{"name", "Shared"}, {"source", "module Shared; public Record B {}"}}}}};
    require(compileProject(prefix, legacy).at("ok") == false, "legacy duplicate module refusal changed");
    legacy["modules"][0]["path"] = "A.pipe";
    const auto legacyPath = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()}, legacy.dump(),
                                    std::chrono::seconds(45), 4U << 20);
    require(legacyPath.exit != 0, "legacy protocol acquired project fragment fields");
}

void largeSdkTransport(const fs::path& prefix, const fs::path& root) {
    // Short paths fit macOS PATH_MAX while enough physical inputs keep the
    // request above 1 MiB and its repeated-path receipts below 4 MiB. The
    // separate protocol test retains the general 4096-part boundary.
    fs::path relative = "domain";
    for (int component = 0; component < 3; ++component)
        relative /= std::string(160, static_cast<char>('a' + component));
    constexpr int fragments = 2048;
    const auto directory = root / "src" / relative;
    fs::create_directories(directory);
    require(fs::is_directory(directory), "large SDK fixture input directory was not created");
    for (int index = 0; index < fragments; ++index) {
        const auto input = directory / ("F" + std::to_string(index) + ".pipe");
        require(!fs::exists(input), "large SDK fixture requires fresh input paths");
        // These are new immutable fixture inputs, consumed only after setup.
        // Write once in place so estate scans never encounter disappearing
        // temporary names from hundreds of unnecessary publication renames.
        std::ofstream output(input, std::ios::binary);
        require(output.is_open(), "failed to open large SDK fixture input: " + input.generic_string());
        output << "module Shared;";
        require(output.good(), "failed to write large SDK fixture input: " + input.generic_string());
        output.close();
        require(output.good(), "failed to close large SDK fixture input: " + input.generic_string());
    }
    const auto entry = relative / "F0.pipe";
    const auto project = SourceProject::load(root, "src", entry);
    require(project.dependencies().size() == fragments, "large SDK fixture lost physical source dependencies");
    const auto requestBytes = project.request("large_module_fixture").dump().size();
    require(requestBytes > (1U << 20) && requestBytes < (4U << 20),
            "large SDK fixture does not exercise the project-only transport budget");
    require(project.information().dump().size() < (4U << 20),
            "large SDK fixture exceeds the unchanged information receipt bound");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    generateProjectBindings(prefix, manifest, fileHash(manifest), nativeExecutable(prefix / "bin/pipelang-modelc"),
                            root, "src", entry, "large_module_fixture", root / "generated");
    const auto receipt = parseJson(readFile(root / "generated/bindings.json", 4U << 20), 4U << 20);
    require(receipt.at("project").at("closure").size() == 1 &&
            receipt.at("project").at("closure_sources").size() == fragments,
            "large SDK project lost module parts during binding generation");
    require(receipt.at("project") == project.receipt(), "large SDK generation changed its source receipt");
    project.verifyUnchanged();
}

void invoke(const std::vector<std::string>& command) {
    const auto response = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(response.exit == 0, "module-fragment installed consumer failed: " + response.output + response.diagnostic);
}

void installedConsumer(const fs::path& prefix, const fs::path& root) {
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(ProjectModuleConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(module_model PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY entry/Entry.pipe
    NAMESPACE module_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(module-consumer main.cpp)
target_link_libraries(module-consumer PRIVATE module_model)
)cmake");
    atomicWrite(root / "main.cpp", R"cpp(#include "Model.hpp"
using module_fixture::source::Gameplay::Start;
using module_fixture::source::Gameplay::CounterValue;
using module_fixture::source::Data::Pair;
using module_fixture::source::Helpers::Extra;
int main() {
    const Start start{Pair{7}, CounterValue{Extra{11}}};
    return start.pair.value == 7 && start.counter.extra.value == 11 ? 0 : 1;
}
)cpp");
    const auto build = root / "build";
    invoke({nativeTool("cmake").string(), "-S", root.string(), "-B", build.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"});
    invoke({nativeExecutable(build / "module-consumer").string()});
    const auto receipt = build / "generated/bindings.json";
    auto previous = fileHash(receipt);
    const auto regenerate = [&] {
        invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "module_model_generate", "--parallel", "1"});
        const auto current = fileHash(receipt);
        require(current != previous, "adding/removing a selected module part did not regenerate its receipt");
        previous = current;
    };
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(root / "src/domain/NewPart.pipe", "module Gameplay; public Record NewPart {}\n");
    regenerate();
    pipelang::tests::waitForBuildTimestampTick(build);
    fs::remove(root / "src/domain/NewPart.pipe");
    regenerate();
}
}

void projectModuleContracts(const std::filesystem::path& prefix, const std::filesystem::path& output) {
    writeFragments(output);
    admissionAndOrigins(prefix, output);
    boundedProtocols(prefix);
    largeSdkTransport(prefix, output / "large-transport");
    installedConsumer(prefix, output);
}
