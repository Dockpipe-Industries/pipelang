// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
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
    require(payload.size() <= 1U << 20, "native request exceeds its capacity");
    const auto result = process({tool.string()}, payload);
    require(result.exit == 0 && result.diagnostic.empty(), "native compiler process failed or emitted diagnostics (exit " + std::to_string(result.exit) + "): " + result.diagnostic);
    auto response = parseJson(result.output, 1U << 20);
    require(response.is_object() && response.contains("protocol") && response.at("protocol") == protocol &&
            response.contains("ok") && response.at("ok").is_boolean(), "unexpected native response protocol or outcome");
    if (!response.at("ok").get<bool>()) {
        exactObject(response, {"protocol", "ok", "diagnostics"}, {}, "native refusal");
        throw std::runtime_error("native compiler refused source: " + response.at("diagnostics").dump());
    }
    std::set<std::string> required{"protocol", "ok", "header", "manifest"};
    if (inspection)
        required.insert("style_descriptor");
    exactObject(response, required, {}, "native response");
    text(response.at("header"), "generated header", 1U << 20);
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
std::string source(const fs::path& path) {
    require(path.is_absolute(), "source must be absolute");
    auto bytes = readFile(path, 65536);
    require(!bytes.empty(), "source cannot be empty");
    return bytes;
}
}

Json generateBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                      const fs::path& modelTool, const fs::path& viewTool,
                      const fs::path& model, const fs::path& view, const std::string& nameSpace,
                      const fs::path& output, bool styleInspection) {
    text(nameSpace, "namespace", 1024);
    outputPreflight(output);
    for (const auto& input : {model, view}) {
        const auto relative = input.lexically_relative(output).generic_string();
        require(input != output && (relative.empty() || relative == ".." || relative.starts_with("../")), "output cannot own an input source");
    }
    const auto sdk = verifyManifest(prefix, manifest, manifestHash);
    std::map<std::string, std::string> hashes;
    for (const auto& [name, tool] : std::map<std::string, fs::path>{{"model", modelTool}, {"view", viewTool}}) {
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
    const auto modelBytes = source(model);
    const auto viewBytes = source(view);
    const auto modelResponse = compile(modelTool, {{"protocol", modelProtocol}, {"source", modelBytes}, {"namespace", nameSpace}}, modelProtocol);
    const auto& modelManifest = modelResponse.at("manifest");
    admitModel(modelManifest, nameSpace);
    requireViewCompatible(modelManifest);
    Json request{{"protocol", viewProtocol}, {"source", viewBytes}, {"model_manifest", modelManifest},
                 {"namespace", nameSpace}, {"model_header", "Model.hpp"}};
    if (styleInspection)
        request["style_inspection"] = styleProtocol;
    const auto viewResponse = compile(viewTool, request, viewProtocol, styleInspection);
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
    require(source(model) == modelBytes && source(view) == viewBytes, "source changed during compilation");
    require(fileHash(modelTool) == hashes.at("model") && fileHash(viewTool) == hashes.at("view"), "native executable changed during compilation");
    verifyManifest(prefix, manifest, manifestHash);
    std::map<std::string, std::string> outputs{
        {"Model.hpp", modelResponse.at("header").get<std::string>()}, {"model.json", canonical(modelManifest)},
        {"ModelView.hpp", viewResponse.at("header").get<std::string>()}, {"view.json", canonical(viewManifest)}
    };
    if (styleInspection)
        outputs["style.json"] = canonical(viewResponse.at("style_descriptor"));
    Json receipt{{"protocol", "pipelang.native-model-bindings.v1"}, {"sdk_version", "0.1.0"}, {"sdk_manifest_sha256", manifestHash},
                 {"namespace", nameSpace}, {"model_source_sha256", sha256(modelBytes)}, {"view_source_sha256", sha256(viewBytes)},
                 {"tools", {{"model", {{"protocol", modelProtocol}, {"sha256", hashes.at("model")}}},
                            {"view", {{"protocol", viewProtocol}, {"sha256", hashes.at("view")}}}}}, {"generated", Json::object()}};
    for (const auto& [name, bytes] : outputs)
        receipt["generated"][name] = sha256(bytes);
    outputs["bindings.json"] = canonical(receipt);
    publishDirectory(output, outputs);
    return receipt;
}
}
