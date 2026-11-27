// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionGraph.hpp"
#include <algorithm>
#include <stdexcept>

namespace pipelang::models {
namespace {

nlohmann::json originJson(const CompositionOrigin& origin) {
    return {{"module", origin.module}, {"path", origin.path}, {"offset", origin.offset}};
}

std::string lifetimeName(ServiceLifetime lifetime) {
    switch (lifetime) {
    case ServiceLifetime::Singleton:
        return "singleton";
    case ServiceLifetime::Scoped:
        return "scoped";
    case ServiceLifetime::Transient:
        return "transient";
    }
    throw std::logic_error("invalid checked composition lifetime");
}

std::string inputName(CompositionInput input) {
    switch (input) {
    case CompositionInput::None:
        return "none";
    case CompositionInput::Snapshot:
        return "snapshot";
    case CompositionInput::Borrowed:
        return "borrowed";
    }
    throw std::logic_error("invalid checked composition input");
}

}

nlohmann::json compositionGraphJson(const CompositionGraph& graph) {
    using Json = nlohmann::json;
    const bool lifecycle = !graph.cleanupContracts.empty() ||
        std::any_of(graph.services.begin(), graph.services.end(), [](const auto& service) {
            return service.factory.has_value();
        });
    Json services = Json::array();
    for (const auto& service : graph.services) {
        Json dependencies = Json::array();
        for (const auto& dependency : service.dependencies) {
            dependencies.push_back({{"parameter", dependency.parameter},
                {"type", dependency.type.displayText()},
                {"service", graph.services.at(dependency.target).service.displayText()},
                {"index", dependency.target}, {"origin", originJson(dependency.origin)}});
        }
        std::string context = "caller";
        if (service.lifetime == ServiceLifetime::Singleton)
            context = "root";
        else if (service.lifetime == ServiceLifetime::Scoped)
            context = "scope";
        services.push_back({{"service", service.service.displayText()},
            {"implementation", service.implementation.displayText()},
            {"lifetime", lifetimeName(service.lifetime)}, {"input", inputName(service.input)},
            {"constructor", service.constructorSignature}, {"origin", originJson(service.origin)},
            {"constructor_origin", originJson(service.constructorOrigin)},
            {"dependencies", std::move(dependencies)},
            {"root_resolvable", service.rootResolvable}, {"scope_resolvable", service.scopeResolvable},
            {"context", context}});
        if (lifecycle) {
            auto& item = services.back();
            item["cleanup_index"] = service.cleanupIndex ? Json(*service.cleanupIndex) : Json(nullptr);
            item["factory"] = nullptr;
            if (service.factory) {
                const auto& factory = *service.factory;
                Json parameters = Json::array();
                for (const auto& parameter : factory.parameters) {
                    parameters.push_back({{"name", parameter.name}, {"type", parameter.type.displayText()},
                        {"index", parameter.target ? Json(*parameter.target) : Json(nullptr)}});
                }
                item["factory"] = {{"receiver", factory.receiver.displayText()},
                    {"receiver_index", factory.receiverIndex}, {"method", factory.method},
                    {"signature", factory.signature}, {"origin", originJson(factory.origin)},
                    {"parameters", std::move(parameters)}};
            }
        }
    }
    Json exports = Json::array();
    for (const auto index : graph.exports)
        exports.push_back(graph.services.at(index).service.displayText());
    Json order = Json::array();
    for (const auto index : graph.constructionOrder)
        order.push_back(graph.services.at(index).service.displayText());
    Json result = {{"protocol", lifecycle ? "pipelang.composition-graph.v2" : "pipelang.composition-graph.v1"}, {"name", graph.name},
        {"origin", originJson(graph.origin)}, {"includes", graph.includes},
        {"services", std::move(services)}, {"exports", std::move(exports)},
        {"construction_order", std::move(order)}, {"checkpoint", "graph-only"}};
    if (lifecycle) {
        result["cleanup_contracts"] = Json::array();
        for (const auto& cleanup : graph.cleanupContracts) {
            result["cleanup_contracts"].push_back({{"contract", cleanup.contract.displayText()},
                {"method", cleanup.method}, {"signature", cleanup.signature},
                {"origin", originJson(cleanup.origin)}, {"selector_origin", originJson(cleanup.selectorOrigin)}});
        }
    }
    return result;
}

}
