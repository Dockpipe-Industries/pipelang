// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompiledProjectTests.hpp"
#include "BuildTimestamp.h"
#include "CompiledImplementationUnitTests.hpp"
#include "ProjectTests.hpp"
#include "../../src/native/sdk-tools/CompiledProjectFiles.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include "../../src/native/sdk-tools/bindings.hpp"

#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace {

using namespace pipelang::tools;

void refused(const std::function<void()>& operation, const std::string& message) {
    try {
        operation();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(message) != std::string::npos,
                "compiled refusal lost expected diagnostic: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("compiled operation accepted " + message);
}

Json compile(const fs::path& compiler, const Json& request) {
    const auto result = process({compiler.string()}, request.dump(), std::chrono::seconds(45), 4U << 20);
    require(result.exit == 0 && result.diagnostic.empty(), "compiled transport failed: " + result.diagnostic);
    return parseJson(result.output, 4U << 20);
}

void invoke(const std::vector<std::string>& command) {
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "compiled fixture command failed: " + result.output + result.diagnostic);
}

}

void compiledProjectContracts(const std::filesystem::path& prefix, const std::filesystem::path& output) {
    using namespace pipelang::tools;
    compiledImplementationUnitContracts();
    generateCompiledProjectFixture(prefix, output);
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto sdkManifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto sdkHash = fileHash(sdkManifest);
    const auto project = SourceProject::load(output, "src", "Entry.pipe");
    auto request = project.request("project_fixture");
    const auto headerOnly = compile(compiler, request);
    request["protocol"] = "pipelang.native-model-project.v2";
    request["emit"] = "sources";
    const auto compiled = compile(compiler, request);
    require(compiled.at("ok") == true && compiled.at("manifest") == headerOnly.at("manifest") &&
            compiled.at("types_header") == headerOnly.at("types_header") &&
            compiled.at("constants_header") == headerOnly.at("constants_header"),
            "compiled mode changed metadata, value schema, constants or source identities");
    require(compiled == compile(compiler, request), "compiled artifact emission is nondeterministic");
    const auto header = compiled.at("header").get<std::string>();
    require(header.find("pl_object_function_") == std::string::npos &&
            header.find("pl_scope.requireOwner();") == std::string::npos &&
            header.find("pl_async_") != std::string::npos,
            "compiled public header retained method implementations or lost declarations");
    const auto files = compiled.at("implementation");
    const auto plan = compiledProjectPlan(files, true);
    auto invalid = files;
    invalid["files"][0]["name"] = "../escaped.cpp";
    refused([&] { compiledProjectPlan(invalid, true); }, "unowned compiled");
    invalid = files;
    invalid["files"].push_back(invalid.at("files").front());
    refused([&] { compiledProjectPlan(invalid, true); }, "duplicate compiled");
    request["emit"] = "plan";
    const auto planned = compile(compiler, request);
    require(compiledProjectPlan(planned.at("implementation"), false) == plan,
            "checked configure plan disagrees with implementation artifacts");
    auto generate = [&](const std::string& identity) {
        return generateCompiledProjectBindings(prefix, sdkManifest, sdkHash, compiler, output,
            "src", "Entry.pipe", "project_fixture", output / "generated", identity);
    };
    const auto identity = sha256(canonical(plan));
    const auto before = fileHash(output / "generated/bindings.json");
    refused([&] { generate(std::string(64, '0')); }, "plan changed");
    require(before == fileHash(output / "generated/bindings.json"), "plan mismatch changed publication");
    const auto headerTime = fs::last_write_time(output / "generated/Model.hpp");
    std::map<std::string, fs::file_time_type> unchangedTimes;
    for (const auto& entry : plan) {
        const auto name = entry.at("name").get<std::string>();
        unchangedTimes.emplace(name, fs::last_write_time(output / "generated" / name));
    }
    generate(identity);
    require(headerTime == fs::last_write_time(output / "generated/Model.hpp"),
            "unchanged artifact publication invalidated consumer object files");
    for (const auto& [name, timestamp] : unchangedTimes)
        require(fs::last_write_time(output / "generated" / name) == timestamp,
                "no-op generation republished unchanged implementation " + name);
    atomicWrite(output / "generated/foreign.txt", "not compiler owned");
    refused([&] { generate(identity); }, "unowned material");
    fs::remove(output / "generated/foreign.txt");
    fs::remove(output / "generated/Types.hpp");
    generate(identity);
    require(fs::exists(output / "generated/Types.hpp"), "missing owned artifact was not regenerated");

    const auto build = output / "build";
    invoke({nativeTool("cmake").string(), "-S", output.string(), "-B", build.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    verifyCompiledProjectPolicy(build, false);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"});
    const auto ctest = nativeExecutable(nativeTool("cmake").parent_path() / "ctest");
    invoke({ctest.string(), "--test-dir", build.string(), "--output-on-failure"});

    const auto pureRoot = output / "pure-host-values";
    const auto pureBuild = pureRoot / "build";
    invoke({nativeTool("cmake").string(), "-S", pureRoot.string(), "-B", pureBuild.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", pureBuild.string(), "--parallel", "1"});
    invoke({ctest.string(), "--test-dir", pureBuild.string(), "--output-on-failure"});

    // A body edit changes only its owning implementation bytes. The complete
    // Source index is still rechecked and the receipt advances independently.
    const auto source = output / "src/domain/Item.pipe";
    const auto sourceBefore = readFile(source, 65536);
    const auto modelTime = fs::last_write_time(build / "generated/Model.hpp");
    std::map<std::string, std::pair<std::string, fs::file_time_type>> implementationBefore;
    for (const auto& entry : fs::directory_iterator(build / "generated")) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("Implementation_") && name.ends_with(".cpp"))
            implementationBefore.emplace(name, std::make_pair(fileHash(entry.path()), fs::last_write_time(entry.path())));
    }
    auto edited = sourceBefore;
    const auto body = edited.find("=> value;");
    require(body != std::string::npos, "compiled mutation fixture lost its method");
    edited.replace(body, std::string("=> value;").size(), "=> value + 0;");
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(source, edited);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "project_model_generate", "--parallel", "1"});
    require(fs::last_write_time(build / "generated/Model.hpp") == modelTime,
            "method body edit republished an unchanged public header timestamp");
    std::size_t changedImplementations = 0;
    std::size_t unchangedImplementations = 0;
    for (const auto& [name, previous] : implementationBefore) {
        const auto path = build / "generated" / name;
        require(fs::exists(path), "small body edit unexpectedly changed the implementation plan");
        if (fileHash(path) == previous.first) {
            ++unchangedImplementations;
            require(fs::last_write_time(path) == previous.second,
                    "body edit republished unrelated implementation " + name);
        } else {
            ++changedImplementations;
        }
    }
    require(changedImplementations != 0 && unchangedImplementations != 0,
            "body mutation did not exercise localized implementation regeneration");

    // Removing an exported class changes the checked plan. Regeneration removes
    // only previously receipted implementation units; no consumer source list.
    const auto extra = output / "src/Extra.pipe";
    const auto entry = output / "src/Entry.pipe";
    const auto originalEntry = readFile(entry, 65536);
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(extra, "module Extra; public model Class Extra {}\n");
    atomicWrite(entry, originalEntry + "import Extra;\n");
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "project_model_generate", "--parallel", "1"});
    const auto added = parseJson(readFile(build / "generated/bindings.json", 4U << 20));
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(entry, originalEntry);
    fs::remove(extra);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "project_model_generate", "--parallel", "1"});
    const auto removed = parseJson(readFile(build / "generated/bindings.json", 4U << 20));
    require(added.at("generated").size() == removed.at("generated").size() + 2,
            "root/object implementation membership did not follow source discovery");
    for (const auto& [name, digest] : added.at("generated").items()) {
        (void)digest;
        if (!removed.at("generated").contains(name))
            require(!fs::exists(build / "generated" / name), "stale owned implementation survived publication");
    }
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"compiled_project_contract", true}, {"multi_translation_unit", true},
        {"source_and_native_facets", true}, {"transactional_artifacts", true}}));
}

void compareCompiledProjectBaseline(const std::filesystem::path& baselineCompiler,
                                    const std::filesystem::path& candidateSdk,
                                    const std::filesystem::path& output) {
    using namespace pipelang::tools;
    generateProjectFixture(candidateSdk, output);
    const auto candidate = nativeExecutable(candidateSdk / "bin/pipelang-modelc");
    const auto project = SourceProject::load(output, "src", "Entry.pipe");
    std::vector<Json> requests{project.request("project_fixture")};
    requests.push_back({{"protocol", "pipelang.native-model.v1"},
        {"source", "public Class Counter { public int value; public Counter(int initial) { value = initial; } public void Set(int next) { value = next; } public pure int Read() => value; }"}});
    const Json modules = Json::array({{{"name", "Library"}, {"source",
        "module Library; public Class Counter { public float32 value; public Counter(float32 initial) { value = initial; } public pure float32 Sum(float32 next) => value + next; }"}}});
    requests.push_back({{"protocol", "pipelang.native-model.v2"}, {"entry", "Library"}, {"modules", modules}});
    requests.push_back({{"protocol", "pipelang.native-model.v3"}, {"entry", "Library"}, {"modules", modules},
                        {"models", Json::array({"Library.Counter"})}});
    auto comparisons = Json::array();
    for (const auto& request : requests) {
        const auto before = compile(baselineCompiler, request);
        const auto after = compile(candidate, request);
        require(before.at("ok") == true && before == after,
                "default emission changed for " + request.at("protocol").get<std::string>());
        comparisons.push_back({{"protocol", request.at("protocol")},
                               {"response_sha256", sha256(canonical(after))}});
    }
    atomicWrite(output / "baseline-comparison.json", canonical({{"status", "passed"},
        {"baseline_compiler_sha256", fileHash(baselineCompiler)}, {"candidate_compiler_sha256", fileHash(candidate)},
        {"comparisons", comparisons}}));
}

void verifyCompiledProjectPolicy(const std::filesystem::path& build, bool sanitizer) {
    using namespace pipelang::tools;
    const auto commands = parseJson(readFile(build / "compile_commands.json", 4U << 20));
    require(commands.is_array() && !commands.empty(), "compiled fixture has no compile commands");
    std::size_t implementations = 0;
    std::size_t consumers = 0;
    std::size_t precompiledHeaders = 0;
    std::size_t unityUnits = 0;
    std::set<fs::path> expectedImplementations;
    std::istringstream sourceList(readFile(build / "implementation-sources.txt", 4U << 20));
    std::string source;
    while (std::getline(sourceList, source)) {
        if (!source.empty()) {
            require(expectedImplementations.insert(fs::path(source).lexically_normal()).second,
                "canonical implementation inventory contains a duplicate");
        }
    }
    require(!expectedImplementations.empty(), "compiled fixture lost its canonical implementation inventory");
    std::set<fs::path> compiledImplementations;
    const std::regex unityInclude(R"include(^#include "([^"]+)")include", std::regex::multiline);
    const auto recordImplementation = [&](const fs::path& implementation) {
        const auto normalized = implementation.lexically_normal();
        require(expectedImplementations.contains(normalized), "compiled an unowned implementation source");
        require(compiledImplementations.insert(normalized).second, "compiled an implementation source more than once");
        ++implementations;
    };
    for (const auto& entry : commands) {
        const auto sourcePath = fs::path(entry.at("file").get<std::string>());
        const auto file = sourcePath.filename().string();
        const bool implementation = file.starts_with("Implementation_");
        if (implementation) {
            recordImplementation(sourcePath);
        } else if (file.starts_with("unity_") && file.ends_with("_cxx.cxx")) {
            const auto contents = readFile(sourcePath, 4U << 20);
            std::size_t members = 0;
            for (auto include = std::sregex_iterator(contents.begin(), contents.end(), unityInclude);
                 include != std::sregex_iterator(); ++include) {
                fs::path member = (*include)[1].str();
                if (!member.is_absolute())
                    member = sourcePath.parent_path() / member;
                recordImplementation(member);
                ++members;
            }
            require(members > 0 && members <= 2, "compiled implementation batch is not a bounded pair");
            ++unityUnits;
        } else if (file == "cmake_pch.hxx.cxx") {
            ++precompiledHeaders;
        } else {
            ++consumers;
        }
        const auto command = entry.at("command").get<std::string>();
        for (const auto& required : {"PIPELANG_COMPILED_DEFINITION=17", "PIPELANG_COMPILED_OPTION=23",
             "PIPELANG_COMPILED_TRANSITIVE_DEFINITION=19", "PIPELANG_COMPILED_TRANSITIVE_OPTION=31",
             "-ffp-contract=fast", "compiled-policy-check.hpp"}) {
            require(command.find(required) != std::string::npos,
                    "producer policy missing from " + file + ": " + required);
        }
        if (sanitizer) {
            for (const auto& required : {"-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
                require(command.find(required) != std::string::npos,
                        "sanitizer policy missing from " + file + ": " + required);
        }
    }
    require(compiledImplementations == expectedImplementations && consumers == 4 &&
            precompiledHeaders == 1 && unityUnits > 0,
            "compiled policy audit missed an owned source, paired build, private header cache or consumer route");
    atomicWrite(build / "compile-policy-acceptance.json", canonical({{"status", "passed"},
        {"instrumented", sanitizer}, {"implementation_units", implementations}, {"consumer_units", consumers},
        {"precompiled_header_units", precompiledHeaders}, {"paired_compile_units", unityUnits}}));
}
