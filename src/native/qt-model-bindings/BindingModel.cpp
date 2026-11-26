// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "BindingModel.hpp"
#include <set>
#include <cctype>
#include <stdexcept>

namespace pipelang::qt_bindings {
bool identifier(const std::string& value) {
    if (value.empty() || value.size() > 96 || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
        return false;
    for (const auto character : value) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_'))
            return false;
    }
    static const std::set<std::string> reserved{"class", "namespace", "template", "public", "private", "protected",
        "return", "void", "bool", "int", "auto", "this", "new", "delete", "operator", "struct", "enum", "while", "for", "if"};
    return !reserved.contains(value);
}
namespace {
std::string text(const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || !value.at(key).is_string())
        throw std::runtime_error("invalid Qt binding metadata text");
    return value.at(key).get<std::string>();
}
bool scalar(const std::string& type) { return type == "bool" || type == "string"; }
void bounded(const nlohmann::json& value) {
    if (!value.is_array() || value.size() > 256)
        throw std::runtime_error("Qt binding metadata array outside profile");
}
}
BindingModel admit(const nlohmann::json& manifest, const std::string& sourceModel) {
    const auto protocol = text(manifest, "protocol");
    if (protocol != "pipelang.native-model-manifest.v1" && protocol != "pipelang.native-model-manifest.v2")
        throw std::runtime_error("unsupported model manifest protocol");
    BindingModel result;
    result.modelNamespace = text(manifest, "namespace");
    if (!identifier(result.modelNamespace))
        throw std::runtime_error("unsafe native model namespace");
    bounded(manifest.at("classes"));
    const nlohmann::json* selected = nullptr;
    for (const auto& candidate : manifest.at("classes")) {
        if (candidate.value("sourceName", text(candidate, "name")) != sourceModel)
            continue;
        if (selected)
            throw std::runtime_error("ambiguous source model identity");
        selected = &candidate;
    }
    if (!selected)
        throw std::runtime_error("Qt model identity is not an exported native model root");
    result.nativeModel = text(*selected, "name");
    result.sourceModel = sourceModel;
    if (!identifier(result.nativeModel))
        throw std::runtime_error("unsafe native model identity");
    std::set<std::string> names{"Model", "successOutcome", "cancelledOutcome", "failureOutcome", "projectionStatus", "projectionChanged", "objectName", "deleteLater", "thread", "destroyed",
        "setParent", "parent", "children", "metaObject", "connect", "disconnect", "property", "setProperty", "event", "eventFilter", "moveToThread",
        "Group", "current", "correlation", "operationPending", "contextStatus", "contextChanged", "operationRetired",
        "latest", "exclusive", "resolveOperation", "finishOperation", "forOperation", "captureTarget", "captureSupervisor", "target", "bindFresh", "bindShared",
        "prototypeIdentity", "refreshProjection"};
    bounded(selected->at("fields"));
    for (const auto& field : selected->at("fields")) {
        const auto name = text(field, "name");
        const auto type = text(field, "type");
        if (!identifier(name) || !names.insert(name).second || !(scalar(type) || type == "List<string>"))
            throw std::runtime_error("Qt properties require unique bool/string public fields or pure getters");
        result.properties.push_back({name, type, false});
    }
    bounded(selected->at("methods"));
    for (const auto& method : selected->at("methods")) {
        const auto name = text(method, "name");
        const auto type = text(method, "returnType");
        if (!identifier(name) || !names.insert(name).second || !method.at("async").is_boolean() ||
            !method.at("pure").is_boolean() || method.at("async").get<bool>())
            throw std::runtime_error("Qt binding profile requires unique synchronous checked methods");
        bounded(method.at("parameters"));
        if (method.at("pure").get<bool>() && method.at("parameters").empty() && (scalar(type) || type == "ContextOwner")) {
            result.properties.push_back({name, type, true});
            continue;
        }
        if (!(scalar(type) || type == "Result<void,string>" || type == "Result<bool,string>" || type == "Result<string,string>"))
            throw std::runtime_error("Qt actions require bool/string or string-error void/bool/string Results");
        Action action{name, type, {}, method.at("pure").get<bool>()};
        std::set<std::string> parameters;
        for (const auto& parameter : method.at("parameters")) {
            const auto parameterName = text(parameter, "name");
            const auto parameterType = text(parameter, "type");
            if (!identifier(parameterName) || !parameters.insert(parameterName).second ||
                !(scalar(parameterType) || parameterType == "List<string>"))
                throw std::runtime_error("Qt action input requires bool/string/List<string>");
            action.parameters.push_back({parameterName, parameterType});
        }
        result.actions.push_back(std::move(action));
    }
    return result;
}
}
