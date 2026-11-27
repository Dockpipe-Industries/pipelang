// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/sdk-tools/CompositionPlanAdmission.hpp"
#include "../../src/native/sdk-tools/bindings.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
using namespace pipelang::models;
using namespace pipelang::tools;
std::size_t checks = 0;

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    require(static_cast<bool>(output), "could not write fixture " + path.string());
}

void check(bool condition, const std::string& message) {
    require(condition, message);
    ++checks;
}

template<class Operation>
void rejects(Operation operation, const std::string& cause) {
    try {
        operation();
    } catch (const std::exception& error) {
        check(std::string(error.what()).find(cause) != std::string::npos,
              "unexpected refusal: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected refusal: " + cause);
}

const std::string source = R"pipe(module Application;
public source Interface IPolicy { public int Value(); }
public Class Policy : IPolicy {
    public Policy() { }
    public int Value() { return 7; }
}
public Class UseCase {
    private IPolicy policy;
    public UseCase(IPolicy dependency) { this.policy = dependency; }
    public int Value() { return policy.Value(); }
}
public composition Startup {
    singleton IPolicy = Policy;
    transient UseCase = UseCase;
    export UseCase;
}
)pipe";

void schemaBoundary() {
    const auto compiled = compileProjectModules({{"Application", source, "Startup.pipe"}}, "Application", "composition_fixture");
    const auto plans = compiled.at("composition_plans");
    admitCompositionPlans(plans);
    ++checks;
    auto bad = plans;
    bad[0]["protocol"] = "pipelang.composition-graph.v99";
    rejects([&] { admitCompositionPlans(bad); }, "protocol");
    bad = plans;
    bad[0]["unexpected"] = true;
    rejects([&] { admitCompositionPlans(bad); }, "composition plan");
    bad = plans;
    bad[0]["origin"]["offset"] = -1;
    rejects([&] { admitCompositionPlans(bad); }, "nonnegative");
    bad = plans;
    bad[0]["services"][0]["lifetime"] = "global";
    rejects([&] { admitCompositionPlans(bad); }, "lifetime");
    bad = plans;
    bad[0]["services"][0]["root_resolvable"] = "true";
    rejects([&] { admitCompositionPlans(bad); }, "boolean");
    bad = plans;
    bad[0]["services"][0]["constructor"] = "";
    rejects([&] { admitCompositionPlans(bad); }, "requires implementation and constructor");
    bad = plans;
    bad[0]["services"][0]["input"] = "borrowed";
    rejects([&] { admitCompositionPlans(bad); }, "construction recipe");
    bad = plans;
    bad[0]["services"][0]["context"] = "scope";
    rejects([&] { admitCompositionPlans(bad); }, "context disagrees");
    bad = plans;
    bad[0]["services"][0]["root_resolvable"] = false;
    rejects([&] { admitCompositionPlans(bad); }, "root availability");
    bad = plans;
    bad[0]["services"][0]["scope_resolvable"] = false;
    rejects([&] { admitCompositionPlans(bad); }, "scope availability");
    bad = plans;
    bad[0]["services"][0]["lifetime"] = "scoped";
    bad[0]["services"][0]["context"] = "scope";
    rejects([&] { admitCompositionPlans(bad); }, "root availability");
    bad = plans;
    auto& dependent = bad[0]["services"][1];
    check(!dependent["dependencies"].empty(), "fixture requires a dependent service");
    dependent["dependencies"][0]["index"] = 999;
    rejects([&] { admitCompositionPlans(bad); }, "index");
    bad = plans;
    std::reverse(bad[0]["construction_order"].begin(), bad[0]["construction_order"].end());
    rejects([&] { admitCompositionPlans(bad); }, "order");
    bad = plans;
    bad[0]["exports"].push_back("Unknown");
    rejects([&] { admitCompositionPlans(bad); }, "export");
    bad = plans;
    bad[0]["includes"].push_back("Unknown.Composition");
    rejects([&] { admitCompositionPlans(bad); }, "missing");
    rejects([&] { admitCompositionPlans(Json::array()); }, "nonempty");
}

void lifecycleSchemaBoundary() {
    const auto compiled = compileProjectModules({{"Application", source, "Startup.pipe"}}, "Application", "composition_fixture");
    auto plans = compiled.at("composition_plans");
    auto& plan = plans.at(0);
    plan["protocol"] = "pipelang.composition-graph.v2";
    plan["cleanup_contracts"] = Json::array({{
        {"contract", "Application.ICleanup"}, {"method", "Application.ICleanup.Close"},
        {"signature", "Application.ICleanup.Close(CancellationToken)->Result<void,string>"},
        {"origin", {{"module", "Application"}, {"path", "ICleanup.pipe"}, {"offset", 0}}},
        {"selector_origin", {{"module", "Application"}, {"path", "Startup.pipe"}, {"offset", 0}}}
    }});
    for (auto& service : plan.at("services")) {
        service["factory"] = nullptr;
        service["cleanup_index"] = nullptr;
    }
    auto& produced = plan.at("services").at(1);
    produced["cleanup_index"] = 0;
    produced["constructor"] = "";
    produced["constructor_origin"] = {{"module", ""}, {"path", ""}, {"offset", 0}};
    const auto receiver = plan.at("services").at(0).at("service");
    produced["dependencies"].at(0)["parameter"] = "$factoryReceiver";
    produced["factory"] = {
        {"receiver", receiver}, {"receiver_index", 0}, {"method", "Application.Policy.Create"},
        {"signature", "Application.Policy.Create(CancellationToken)->Result<Application.UseCase,string>"},
        {"origin", {{"module", "Application"}, {"path", "Factory.pipe"}, {"offset", 0}}},
        {"parameters", Json::array({{{"name", "cancellation"}, {"type", "CancellationToken"}, {"index", nullptr}}})}
    };
    admitCompositionPlans(plans);
    ++checks;
    auto bad = plans;
    bad[0]["cleanup_contracts"][0]["unexpected"] = true;
    rejects([&] { admitCompositionPlans(bad); }, "cleanup contract");
    bad = plans;
    bad[0]["cleanup_contracts"].push_back(bad[0]["cleanup_contracts"][0]);
    rejects([&] { admitCompositionPlans(bad); }, "duplicate");
    bad = plans;
    bad[0]["services"][1]["cleanup_index"] = 1;
    rejects([&] { admitCompositionPlans(bad); }, "unknown contract");
    bad = plans;
    bad[0]["services"][1]["factory"]["receiver_index"] = 999;
    rejects([&] { admitCompositionPlans(bad); }, "unknown service");
    bad = plans;
    bad[0]["services"][1]["dependencies"][0]["parameter"] = "implicit";
    rejects([&] { admitCompositionPlans(bad); }, "receiver dependency");
    bad = plans;
    bad[0]["services"][1]["factory"]["parameters"][0]["type"] = "int";
    rejects([&] { admitCompositionPlans(bad); }, "CancellationToken");
    bad = plans;
    bad[0]["services"][1]["factory"]["parameters"].push_back(
        {{"name", "other"}, {"type", "CancellationToken"}, {"index", nullptr}});
    rejects([&] { admitCompositionPlans(bad); }, "CancellationToken");
    bad = plans;
    bad[0]["services"][1]["constructor"] = "Application.UseCase()";
    rejects([&] { admitCompositionPlans(bad); }, "exactly one");
    bad = plans;
    bad[0]["services"][1]["factory"] = false;
    rejects([&] { admitCompositionPlans(bad); }, "null or an object");
    bad = plans;
    bad[0]["protocol"] = "pipelang.composition-graph.v1";
    rejects([&] { admitCompositionPlans(bad); }, "composition plan");

    auto withParameter = plans;
    auto parameterService = withParameter[0]["services"][0];
    parameterService["service"] = "Application.Configuration";
    parameterService["implementation"] = "";
    parameterService["constructor"] = "";
    parameterService["constructor_origin"] = {{"module", ""}, {"path", ""}, {"offset", 0}};
    parameterService["input"] = "snapshot";
    withParameter[0]["services"].push_back(parameterService);
    withParameter[0]["construction_order"].insert(
        withParameter[0]["construction_order"].begin() + 1, "Application.Configuration");
    auto& parameterFactory = withParameter[0]["services"][1];
    parameterFactory["factory"]["parameters"].push_back(
        {{"name", "configuration"}, {"type", "Application.Configuration"}, {"index", 2}});
    parameterFactory["dependencies"].push_back({
        {"parameter", "configuration"}, {"type", "Application.Configuration"},
        {"service", "Application.Configuration"}, {"index", 2},
        {"origin", {{"module", "Application"}, {"path", "Factory.pipe"}, {"offset", 0}}}
    });
    admitCompositionPlans(withParameter);
    ++checks;
    bad = withParameter;
    bad[0]["services"][1]["factory"]["parameters"][1]["index"] = 0;
    rejects([&] { admitCompositionPlans(bad); }, "ordered dependency");
    bad = withParameter;
    bad[0]["services"][1]["factory"]["parameters"].erase(1);
    rejects([&] { admitCompositionPlans(bad); }, "unmatched dependencies");
    bad = withParameter;
    bad[0]["services"][2]["cleanup_index"] = 0;
    rejects([&] { admitCompositionPlans(bad); }, "input cannot be owned");
    bad = plans;
    bad[0]["cleanup_contracts"][0]["selector_origin"]["offset"] = -1;
    rejects([&] { admitCompositionPlans(bad); }, "nonnegative");
}

void installedSdk(const fs::path& sdk, const fs::path& proof) {
    fs::create_directories(proof / "src");
    writeFile(proof / "src/Startup.pipe", source);
    const auto manifest = sdk / "share/pipelang-model/sdk-manifest.json";
    const auto manifestHash = fileHash(manifest);
    const auto output = proof / "generated";
    const auto receipt = generateProjectBindings(sdk, manifest, manifestHash, nativeExecutable(sdk / "bin/pipelang-modelc"),
        proof, "src", "Startup.pipe", "composition_fixture", output, {}, true);
    check(receipt.at("generated").contains("CompositionPlan.json"), "composition plan missing from artifact receipt");
    const auto plan = parseJson(readFile(output / "CompositionPlan.json", resourceBudget().outputBytes));
    admitCompositionPlans(plan);
    check(fileHash(output / "CompositionPlan.json") ==
              receipt.at("generated").at("CompositionPlan.json").get<std::string>(),
          "composition plan digest mismatch");
    writeFile(proof / "src/Startup.pipe", "module Application; public Class Plain { public Plain() {} }");
    rejects([&] {
        generateProjectBindings(sdk, manifest, manifestHash, nativeExecutable(sdk / "bin/pipelang-modelc"),
            proof, "src", "Startup.pipe", "composition_fixture", proof / "missing", {}, true);
    }, "required composition plan is absent");
    check(!fs::exists(proof / "missing/CompositionPlan.json"), "failed plan requirement published artifacts");
    const auto plainReceipt = generateProjectBindings(sdk, manifest, manifestHash, nativeExecutable(sdk / "bin/pipelang-modelc"),
        proof, "src", "Startup.pipe", "composition_fixture", output);
    check(!plainReceipt.at("generated").contains("CompositionPlan.json") &&
          !fs::exists(output / "CompositionPlan.json"),
          "project without compositions retained stale plan metadata");

    writeFile(proof / "src/Startup.pipe", source);
    writeFile(proof / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.20)
project(composition_artifact LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(application PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src ENTRY Startup.pipe NAMESPACE composition_fixture
    COMPOSITION_PLAN OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
)cmake");
    const auto build = proof / "build";
    const auto cmake = nativeTool("cmake").string();
    const std::string generator =
#ifdef _WIN32
        "Ninja";
#else
        "Unix Makefiles";
#endif
    const auto configured = process({cmake, "-S", proof.string(), "-B", build.string(), "-G", generator,
        "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
        "-DCMAKE_PREFIX_PATH=" + sdk.string()}, {}, std::chrono::seconds(30));
    check(configured.exit == 0, "composition CMake configuration failed: " + configured.output + configured.diagnostic);
    const auto buildCommand = std::vector<std::string>{cmake, "--build", build.string(),
        "--parallel", "1", "--target", "application_generate"};
    auto built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0, "composition CMake generation failed: " + built.output + built.diagnostic);
    const auto generated = build / "generated/CompositionPlan.json";
    check(fs::exists(generated), "CMake declared composition artifact was not generated");
    const auto stamp = fs::last_write_time(generated);
    built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0 && fs::last_write_time(generated) == stamp,
          "unchanged composition project regenerated: " + built.output);
    fs::remove(generated);
    built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0 && fs::exists(generated), "missing declared composition artifact did not regenerate");
}

void factoryArtifacts(const fs::path& sdk, const fs::path& proof) {
    fs::create_directories(proof / "src");
    writeFile(proof / "src/Startup.pipe", source);
    const auto manifest = sdk / "share/pipelang-model/sdk-manifest.json";
    const auto manifestHash = fileHash(manifest);
    const auto compiler = nativeExecutable(sdk / "bin/pipelang-modelc");
    const auto output = proof / "generated";
    const auto receipt = generateProjectBindings(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture", output, {}, true, true);
    check(receipt.at("composition_emission") == "factory", "factory selection missing from receipt");
    check(receipt.at("generated").contains("CompositionFactory.hpp"), "factory artifact missing from receipt");
    check(fileHash(output / "CompositionFactory.hpp") ==
              receipt.at("generated").at("CompositionFactory.hpp").get<std::string>(),
          "factory artifact digest mismatch");
    const auto ordinary = generateProjectBindings(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture", output);
    check(!ordinary.contains("composition_emission") && !fs::exists(output / "CompositionFactory.hpp"),
          "ordinary generation retained a stale optional factory");
    check(fs::exists(output / "CompositionPlan.json"), "disabling factory removed the source composition plan");

    const auto information = compiledProjectInformation(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture", {}, true);
    const auto ordinaryInformation = compiledProjectInformation(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture");
    check(information.at("composition_emission") == "factory" &&
          information.at("plan_sha256") != ordinaryInformation.at("plan_sha256"),
          "compiled plan identity ignored factory selection");
    const auto compiledOutput = proof / "compiled";
    rejects([&] {
        generateCompiledProjectBindings(sdk, manifest, manifestHash, compiler, proof, "src", "Startup.pipe",
            "composition_fixture", compiledOutput, ordinaryInformation.at("plan_sha256"), {}, true, true);
    }, "plan changed");
    check(!fs::exists(compiledOutput / "CompositionFactory.hpp"), "mismatched plan published factory output");
    const auto compiled = generateCompiledProjectBindings(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture", compiledOutput,
        information.at("plan_sha256"), {}, true, true);
    check(compiled.at("generated").contains("Objects.hpp") &&
          compiled.at("generated").contains("CompositionFactory.hpp"),
          "compiled factory omitted checked object declarations or factory header");
    generateCompiledProjectBindings(sdk, manifest, manifestHash, compiler,
        proof, "src", "Startup.pipe", "composition_fixture", compiledOutput,
        ordinaryInformation.at("plan_sha256"));
    check(!fs::exists(compiledOutput / "CompositionFactory.hpp"),
          "compiled generation retained stale factory output");

    writeFile(proof / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.20)
project(composition_factory_artifact LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
option(ENABLE_FACTORY "Generate explicit composition factory" ON)
if(ENABLE_FACTORY)
    set(factory_argument COMPOSITION_FACTORY)
endif()
pipelang_add_model(application PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src ENTRY Startup.pipe NAMESPACE composition_fixture
    COMPOSITION_PLAN ${factory_argument} OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
)cmake");
    const auto build = proof / "build";
    const auto cmake = nativeTool("cmake").string();
    const std::string generator =
#ifdef _WIN32
        "Ninja";
#else
        "Unix Makefiles";
#endif
    auto configureCommand = std::vector<std::string>{cmake, "-S", proof.string(), "-B", build.string(),
        "-G", generator, "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
        "-DCMAKE_PREFIX_PATH=" + sdk.string()};
    auto configured = process(configureCommand, {}, std::chrono::seconds(30));
    check(configured.exit == 0, "factory CMake configuration failed: " + configured.output + configured.diagnostic);
    const auto buildCommand = std::vector<std::string>{cmake, "--build", build.string(),
        "--parallel", "1", "--target", "application_generate"};
    auto built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0, "factory CMake generation failed: " + built.output + built.diagnostic);
    const auto factory = build / "generated/CompositionFactory.hpp";
    check(fs::exists(factory), "CMake required factory output was not generated");
    const auto stamp = fs::last_write_time(factory);
    built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0 && fs::last_write_time(factory) == stamp, "unchanged factory regenerated");
    fs::remove(factory);
    built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0 && fs::exists(factory), "missing required factory did not regenerate");
    configureCommand.push_back("-DENABLE_FACTORY=OFF");
    configured = process(configureCommand, {}, std::chrono::seconds(30));
    check(configured.exit == 0, "factory deselection configuration failed");
    built = process(buildCommand, {}, std::chrono::seconds(30));
    check(built.exit == 0 && !fs::exists(factory), "CMake factory deselection retained stale output");

    writeFile(proof / "src/Startup.pipe", "module Application; public Class Plain { public Plain() {} }");
    rejects([&] {
        generateProjectBindings(sdk, manifest, manifestHash, compiler, proof, "src", "Startup.pipe",
            "composition_fixture", proof / "missing", {}, true, true);
    }, "composition");
    check(!fs::exists(proof / "missing/Model.hpp"), "missing composition published partial factory artifacts");
}
}

int main(int argc, char** argv) {
    try {
        schemaBoundary();
        lifecycleSchemaBoundary();
        if (argc == 3) {
            installedSdk(fs::absolute(argv[1]), fs::absolute(argv[2]));
            factoryArtifacts(fs::absolute(argv[1]), fs::absolute(argv[2]) / "factory");
        } else {
            require(argc == 1, "usage: composition-sdk-tests [SDK PROOF]");
        }
        std::cout << checks << " composition SDK checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
