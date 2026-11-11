// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
#include "ResourceBudgetReceipt.hpp"
#include "../model-compiler/ScopedResourceBudget.hpp"
#include "source_set.hpp"
#include "SourceProject.hpp"
#include "CompiledProjectFiles.hpp"
#include "CompositionPlanAdmission.hpp"
#include <optional>
#include <algorithm>
#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace pipelang::tools {
namespace {
constexpr const char* modelProtocol = "pipelang.native-model.v1";
constexpr const char* viewProtocol = "pipelang.native-model-view.v1";
constexpr const char* styleProtocol = "pipelang.native-style-descriptor.v1";
Json compile(const fs::path& tool, const Json& request, const std::string& protocol, bool inspection = false) {
    const auto payload = request.dump();
    const bool ordinaryModel = protocol.starts_with("pipelang.native-model.") ||
                               protocol.starts_with("pipelang.native-model-project.");
    const std::size_t requestLimit = ordinaryModel ? models::resourceBudget().inputBytes : 1U << 20;
    require(payload.size() <= requestLimit,
            "native request input_bytes resource budget exceeded; configure input_bytes");
    const std::size_t responseLimit = ordinaryModel ? models::resourceBudget().outputBytes : 1U << 20;
    std::vector<std::string> command{tool.string()};
    if (ordinaryModel) {
        command.push_back("--resource-budget-json");
        command.push_back(models::resourceBudget().json().dump());
    }
    const auto result = process(command, payload, std::chrono::seconds(45), responseLimit);
    require(result.exit == 0 && result.diagnostic.empty(), "native compiler process failed or emitted diagnostics (exit " + std::to_string(result.exit) + "): " + result.diagnostic);
    auto response = parseJson(result.output, responseLimit);
    require(response.is_object() && response.contains("protocol") && response.at("protocol") == protocol &&
            response.contains("ok") && response.at("ok").is_boolean(), "unexpected native response protocol or outcome");
    if (!response.at("ok").get<bool>()) {
        exactObject(response, {"protocol", "ok", "diagnostics"}, {}, "native refusal");
        throw std::runtime_error("native compiler refused source: " + response.at("diagnostics").dump());
    }
    std::set<std::string> required{"protocol", "ok", "header", "manifest"};
    if (inspection)
        required.insert("style_descriptor");
    if (protocol == "pipelang.native-model-project.v1" || protocol == "pipelang.native-model-project.v2") {
        required.insert("types_header");
        required.insert("numeric_header");
    }
    if (protocol == "pipelang.native-model-project.v2")
        required.insert("implementation");
    std::set<std::string> optional = protocol == viewProtocol
        ? std::set<std::string>{} : std::set<std::string>{"constants_header"};
    if (protocol.starts_with("pipelang.native-model-project.")) {
        optional.insert("composition_plans");
        optional.insert("composition_factory_header");
    }
    exactObject(response, required, optional, "native response");
    if (response.contains("composition_plans"))
        admitCompositionPlans(response.at("composition_plans"));
    const bool factoryRequested = request.value("composition_emission", std::string{}) == "factory";
    require(response.contains("composition_factory_header") == factoryRequested,
            "composition factory artifact must match its explicit request");
    if (factoryRequested) {
        require(response.contains("composition_plans"), "composition factory requires a checked composition plan");
        require(!text(response.at("composition_factory_header"), "composition factory header", responseLimit).empty(),
                "composition factory header cannot be empty");
    }
    if (response.contains("constants_header"))
        text(response.at("constants_header"), "standalone constants header", responseLimit);
    if (response.contains("types_header"))
        text(response.at("types_header"), "generated project types header", responseLimit);
    if (response.contains("numeric_header"))
        text(response.at("numeric_header"), "generated caller-host numeric header", responseLimit);
    text(response.at("header"), "generated header", responseLimit);
    return response;
}
void admitStyle(const Json& descriptor, const Json& view) {
    exactObject(descriptor, {"protocol", "view", "style", "scope", "properties"}, {}, "style descriptor");
    require(descriptor.at("protocol") == styleProtocol && descriptor.at("view") == view.at("name") &&
            descriptor.at("scope") == "resolved-authored", "style descriptor protocol, scope or view mismatch");
    if (!descriptor.at("style").is_null())
        text(descriptor.at("style"), "selected style");
    const auto& properties = descriptor.at("properties");
    require(properties.is_array() && properties.size() <= 14, "invalid style property inventory");
    require(!descriptor.at("style").is_null() || properties.empty(), "unselected style has properties");
    const std::map<std::string, std::pair<double, double>> lengths{
        {"padding", {0, 64}}, {"gap", {0, 64}}, {"textInset", {0, 64}}, {"labelHeight", {1, 128}},
        {"controlHeight", {1, 256}}, {"fontPixelHeight", {7, 56}}, {"borderWidth", {0, 8}}
    };
    const std::set<std::string> colors{"background", "control", "field", "border", "focus", "text", "disabledText"};
    std::string previous;
    for (const auto& property : properties) {
        require(property.is_object() && property.contains("name"), "invalid style property");
        const auto name = text(property.at("name"), "style property name", 1024);
        require(previous.empty() || previous < name, "style properties must be unique and sorted");
        previous = name;
        if (lengths.contains(name)) {
            exactObject(property, {"name", "type", "unit", "value"}, {}, "style length");
            require(property.at("type") == "length" && property.at("unit") == "px" && property.at("value").is_number(), "invalid style length type or unit");
            const auto value = property.at("value").get<double>();
            const auto bounds = lengths.at(name);
            require(std::isfinite(value) && value >= bounds.first && value <= bounds.second, "style length exceeds its typed bounds");
        } else {
            exactObject(property, {"name", "type", "value"}, {}, "style color");
            const auto value = text(property.at("value"), "style color", 9);
            require(colors.contains(name) && property.at("type") == "color" && value.size() == 9 && value[0] == '#' &&
                std::all_of(value.begin() + 1, value.end(), [](char digit) {
                    return (digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F');
                }), "unsupported or noncanonical style color");
        }
    }
}
std::string viewSourceFormat(const fs::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".pview")
        return "pview";
    if (extension == ".pxml")
        return "pxml";
    if (extension == ".vpipe")
        throw std::runtime_error(".vpipe is not a PipeView source extension; use .pview or .pxml");
    if (extension == ".pipe")
        throw std::runtime_error("VIEW sources use .pview or .pxml; ordinary MODEL sources retain .pipe");
    throw std::runtime_error("VIEW source must have the .pview or .pxml extension");
}

std::string source(const fs::path& path) {
    require(path.is_absolute(), "source must be absolute");
    auto bytes = readFile(path, models::resourceBudget().inputBytes);
    require(!bytes.empty(), "source cannot be empty");
    return bytes;
}
}

namespace {
Json generateArtifacts(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                      const fs::path& modelTool, const fs::path& viewTool,
                      const fs::path& model, const fs::path& view, const std::string& nameSpace,
                      const fs::path& output, bool styleInspection, bool withView, const fs::path& descriptor = {},
                      const fs::path& projectRoot = {}, const fs::path& sourceRoot = {},
                      const fs::path& entry = {}, bool compiled = false, bool planning = false,
                      const std::string& expectedPlan = {}, const std::string& numericEnvironment = {},
                      bool requireCompositionPlan = false, bool emitCompositionFactory = false) {
    require(!requireCompositionPlan || !projectRoot.empty(), "composition plans require PROJECT_ROOT");
    require(!emitCompositionFactory || (!projectRoot.empty() && !withView),
            "composition factories require a headless PROJECT_ROOT");
    require(numericEnvironment.empty() || (numericEnvironment == "active-host" && !projectRoot.empty()),
            "native numeric environment requires an active-host project selection");
    require(withView || !styleInspection, "headless model generation cannot inspect view styles");
    const auto viewFormat = withView ? viewSourceFormat(view) : std::string{};
    require(descriptor.empty() || (!withView && model.empty()), "source sets are headless inputs, exclusive with MODEL");
    require(projectRoot.empty() || (!withView && model.empty() && descriptor.empty()),
            "projects are headless inputs, exclusive with MODEL and SOURCE_SET");
    text(nameSpace, "namespace", 1024);
    require(!compiled || !projectRoot.empty(), "compiled artifacts require PROJECT_ROOT");
    require(!planning || compiled, "implementation planning requires compiled mode");
    if (!planning) {
        if (compiled)
            compiledProjectOwnedNames(output);
        else
            outputPreflight(output);
    }
    std::vector<fs::path> inputs;
    if (!model.empty())
        inputs.push_back(model);
    if (!descriptor.empty())
        inputs.push_back(descriptor);
    if (withView)
        inputs.push_back(view);
    for (const auto& input : inputs) {
        const auto relative = input.lexically_relative(output).generic_string();
        require(input != output && (relative.empty() || relative == ".." || relative.starts_with("../")), "output cannot own an input source");
    }
    const auto sdk = verifyManifest(prefix, manifest, manifestHash);
    std::map<std::string, std::string> hashes;
    std::map<std::string, fs::path> tools{{"model", modelTool}};
    if (withView)
        tools.emplace("view", viewTool);
    for (const auto& [name, tool] : tools) {
        require(tool.is_absolute(), "native tool must be absolute");
        regularFile(tool);
#ifndef _WIN32
        require(::access(tool.c_str(), X_OK) == 0, "native tool must be executable");
#endif
        const auto relative = tool.lexically_relative(prefix).generic_string();
        portablePath(relative);
        hashes[name] = fileHash(tool);
        const auto found = std::find_if(sdk.at("files").begin(), sdk.at("files").end(), [&](const auto& file) {
            return file.at("path") == relative && file.at("sha256") == hashes.at(name);
        });
        require(found != sdk.at("files").end(), "native tool is absent from the pinned SDK inventory");
    }
    std::optional<SourceSet> sourceSet;
    if (!descriptor.empty()) {
        sourceSet.emplace(SourceSet::load(descriptor));
        for (const auto& input : sourceSet->dependencies()) {
            const auto relative = input.lexically_relative(output).generic_string();
            require(input != output && (relative.empty() || relative == ".." || relative.starts_with("../")),
                    "output cannot own a source-set input");
        }
    }
    std::optional<SourceProject> project;
    if (!projectRoot.empty()) {
        project.emplace(SourceProject::load(projectRoot, sourceRoot, entry));
        for (const auto& input : planning ? std::vector<fs::path>{} : project->dependencies()) {
            const auto relative = input.lexically_relative(output).generic_string();
            require(input != output && (relative.empty() || relative == ".." || relative.starts_with("../")),
                    "output cannot own a project input");
        }
        const auto sourceDirectory = projectRoot / sourceRoot;
        const auto outputRelative = output.lexically_relative(sourceDirectory).generic_string();
        require(planning || outputRelative.empty() || outputRelative == ".." || outputRelative.starts_with("../"),
                "project outputs must be outside the discovered source root");
    }
    const auto modelBytes = sourceSet || project ? std::string{} : source(model);
    const auto viewBytes = withView ? source(view) : std::string{};
    std::string selectedProtocol = modelProtocol;
    Json request{{"protocol", modelProtocol}, {"source", modelBytes}, {"namespace", nameSpace}};
    if (sourceSet) {
        selectedProtocol = sourceSet->compilerProtocol();
        request = sourceSet->request(nameSpace);
    } else if (project) {
        selectedProtocol = "pipelang.native-model-project.v1";
        request = project->request(nameSpace);
        if (compiled) {
            selectedProtocol = "pipelang.native-model-project.v2";
            request["protocol"] = selectedProtocol;
            request["emit"] = planning ? "plan" : "sources";
        }
    }
    if (!numericEnvironment.empty())
        request["native_numeric_environment"] = numericEnvironment;
    if (emitCompositionFactory)
        request["composition_emission"] = "factory";
    const auto modelResponse = compile(modelTool, request, selectedProtocol);
    Json implementationPlan;
    std::string planIdentity;
    if (compiled) {
        implementationPlan = compiledProjectPlan(modelResponse.at("implementation"), !planning);
        Json identity = implementationPlan;
        if (!numericEnvironment.empty() || emitCompositionFactory) {
            identity = Json{{"implementation", implementationPlan}};
            if (!numericEnvironment.empty())
                identity["native_numeric_environment"] = numericEnvironment;
            if (emitCompositionFactory)
                identity["composition_emission"] = "factory";
        }
        planIdentity = sha256(canonical(identity));
        require(planning || expectedPlan == planIdentity,
                "compiled project plan changed; reconfigure the owning CMake target");
    }
    const auto& modelManifest = modelResponse.at("manifest");
    admitModel(modelManifest, nameSpace);
    require(modelManifest.value("nativeNumericEnvironment", "") == numericEnvironment,
            "compiler changed the selected native numeric environment");
    require(modelResponse.contains("constants_header") == modelManifest.contains("constants"),
        "constant exports and standalone header must be published together");
    if (sourceSet || project) {
        const auto expectedManifest = project || selectedProtocol == "pipelang.native-model.v3"
            ? "pipelang.native-model-manifest.v2" : "pipelang.native-model-manifest.v1";
        require(modelManifest.at("protocol") == expectedManifest,
                "source-set compiler returned the wrong model manifest profile");
    }
    Json viewResponse;
    if (withView) {
        requireViewCompatible(modelManifest);
        Json request{{"protocol", viewProtocol}, {"source", viewBytes}, {"model_manifest", modelManifest},
                     {"namespace", nameSpace}, {"model_header", "Model.hpp"}};
        request["source_format"] = viewFormat;
        if (styleInspection)
            request["style_inspection"] = styleProtocol;
        viewResponse = compile(viewTool, request, viewProtocol, styleInspection);
        const auto& viewManifest = viewResponse.at("manifest");
        exactObject(viewManifest, {"protocol", "name", "adapter", "model", "namespace"}, {}, "view manifest");
        require(viewManifest.at("protocol") == "pipelang.native-model-view-manifest.v1" && viewManifest.at("namespace") == nameSpace,
                "view manifest protocol or namespace mismatch");
        for (const auto& name : {"name", "adapter", "model"})
            text(viewManifest.at(name), std::string("view ") + name, 1024);
        require(std::any_of(modelManifest.at("classes").begin(), modelManifest.at("classes").end(), [&](const auto& modelClass) {
            return modelClass.at("name") == viewManifest.at("model");
        }), "view refers to an unadmitted model");
        if (styleInspection)
            admitStyle(viewResponse.at("style_descriptor"), viewManifest);
    }
    if (sourceSet)
        sourceSet->verifyUnchanged();
    else if (project)
        project->verifyUnchanged();
    else
        require(source(model) == modelBytes && (!withView || source(view) == viewBytes), "source changed during compilation");
    for (const auto& [name, tool] : tools)
        require(fileHash(tool) == hashes.at(name), "native executable changed during compilation");
    verifyManifest(prefix, manifest, manifestHash);
    if (planning) {
        auto information = project->information();
        information["protocol"] = "pipelang.native-model-project-compile-info.v1";
        information["implementation"] = implementationPlan;
        information["plan_sha256"] = planIdentity;
        if (!numericEnvironment.empty())
            information["native_numeric_environment"] = numericEnvironment;
        if (emitCompositionFactory)
            information["composition_emission"] = "factory";
        information["resource_budget"] = resourceBudgetReceipt();
        require(canonical(information).size() <= models::resourceBudget().outputBytes,
                "compiled project information output_bytes resource budget exceeded; configure output_bytes");
        return information;
    }
    require(!requireCompositionPlan || modelResponse.contains("composition_plans"),
            "required composition plan is absent; author a project composition declaration");
    std::map<std::string, std::string> outputs{
        {"Model.hpp", modelResponse.at("header").get<std::string>()}, {"model.json", canonical(modelManifest)},
        {"StandaloneConstants.hpp", modelResponse.value("constants_header", std::string("#pragma once\n"))}
    };
    if (modelResponse.contains("composition_plans"))
        outputs["CompositionPlan.json"] = canonical(modelResponse.at("composition_plans"));
    if (emitCompositionFactory)
        outputs["CompositionFactory.hpp"] = modelResponse.at("composition_factory_header").get<std::string>();
    if (modelResponse.contains("types_header"))
        outputs["Types.hpp"] = modelResponse.at("types_header").get<std::string>();
    if (modelResponse.contains("numeric_header"))
        outputs["Numeric.hpp"] = modelResponse.at("numeric_header").get<std::string>();
    if (compiled) {
        for (const auto& file : modelResponse.at("implementation").at("files"))
            outputs.emplace(file.at("name").get<std::string>(), file.at("content").get<std::string>());
    }
    if (withView) {
        outputs["ModelView.hpp"] = viewResponse.at("header").get<std::string>();
        outputs["view.json"] = canonical(viewResponse.at("manifest"));
    }
    if (styleInspection)
        outputs["style.json"] = canonical(viewResponse.at("style_descriptor"));
    Json receipt{{"protocol", withView ? "pipelang.native-model-bindings.v1" : "pipelang.native-model-artifacts.v1"},
                 {"sdk_version", "0.1.0"}, {"sdk_manifest_sha256", manifestHash}, {"namespace", nameSpace},
                 {"model_source_sha256", sha256(modelBytes)},
                 {"tools", {{"model", {{"protocol", modelProtocol}, {"sha256", hashes.at("model")}}}}},
                 {"generated", Json::object()}};
    receipt["resource_budget"] = resourceBudgetReceipt();
    if (!numericEnvironment.empty())
        receipt["native_numeric_environment"] = numericEnvironment;
    if (emitCompositionFactory)
        receipt["composition_emission"] = "factory";
    if (sourceSet) {
        receipt["protocol"] = "pipelang.native-model-artifacts.v2";
        receipt.erase("model_source_sha256");
        receipt["source_set"] = sourceSet->receipt();
        receipt["tools"]["model"]["protocol"] = selectedProtocol;
    }
    if (project) {
        receipt["protocol"] = compiled ? "pipelang.native-model-artifacts.v4" : "pipelang.native-model-artifacts.v3";
        if (compiled)
            receipt["implementation_plan_sha256"] = planIdentity;
        receipt.erase("model_source_sha256");
        receipt["project"] = project->receipt();
        receipt["tools"]["model"]["protocol"] = selectedProtocol;
    }
    if (withView) {
        receipt["view_source_sha256"] = sha256(viewBytes);
        receipt["tools"]["view"] = {{"protocol", viewProtocol}, {"sha256", hashes.at("view")}};
    }
    for (const auto& [name, bytes] : outputs)
        receipt["generated"][name] = sha256(bytes);
    outputs["bindings.json"] = canonical(receipt);
    if (sourceSet)
        sourceSet->verifyUnchanged();
    if (project)
        project->verifyUnchanged();
    verifyResourceBudgetUnchanged();
    if (compiled)
        publishCompiledProject(output, outputs);
    else
        publishDirectory(output, outputs);
    return receipt;
}
}

Json generateBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                      const fs::path& modelTool, const fs::path& viewTool,
                      const fs::path& model, const fs::path& view, const std::string& nameSpace,
                      const fs::path& output, bool styleInspection) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, viewTool,
        model, view, nameSpace, output, styleInspection, true);
}

Json generateSourceSetBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                              const fs::path& modelTool, const fs::path& descriptor,
                              const std::string& nameSpace, const fs::path& output) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, {},
        {}, {}, nameSpace, output, false, false, descriptor);
}

Json generateProjectBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                             const fs::path& modelTool, const fs::path& projectRoot,
                             const fs::path& sourceRoot, const fs::path& entry,
                             const std::string& nameSpace, const fs::path& output,
                             const std::string& numericEnvironment, bool requireCompositionPlan,
                             bool emitCompositionFactory) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, {},
        {}, {}, nameSpace, output, false, false, {}, projectRoot, sourceRoot, entry, false, false, {}, numericEnvironment,
        requireCompositionPlan, emitCompositionFactory);
}

Json generateModelBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                           const fs::path& modelTool, const fs::path& model,
                           const std::string& nameSpace, const fs::path& output) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, {},
        model, {}, nameSpace, output, false, false);
}
Json compiledProjectInformation(const fs::path& prefix, const fs::path& manifest,
                                const std::string& manifestHash, const fs::path& modelTool,
                                const fs::path& projectRoot, const fs::path& sourceRoot,
                                const fs::path& entry, const std::string& nameSpace,
                                const std::string& numericEnvironment, bool emitCompositionFactory) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, {},
        {}, {}, nameSpace, {}, false, false, {}, projectRoot, sourceRoot, entry, true, true, {}, numericEnvironment,
        emitCompositionFactory, emitCompositionFactory);
}

Json generateCompiledProjectBindings(const fs::path& prefix, const fs::path& manifest,
                                     const std::string& manifestHash, const fs::path& modelTool,
                                     const fs::path& projectRoot, const fs::path& sourceRoot,
                                     const fs::path& entry, const std::string& nameSpace,
                                     const fs::path& output, const std::string& expectedPlan,
                                     const std::string& numericEnvironment, bool requireCompositionPlan,
                                     bool emitCompositionFactory) {
    return generateArtifacts(prefix, manifest, manifestHash, modelTool, {},
        {}, {}, nameSpace, output, false, false, {}, projectRoot, sourceRoot, entry, true, false, expectedPlan, numericEnvironment,
        requireCompositionPlan, emitCompositionFactory);
}

}
