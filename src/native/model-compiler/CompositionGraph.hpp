// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompositionSyntax.hpp"
#include "../third_party/nlohmann/json.hpp"
#include <string>
#include <optional>
#include <vector>

namespace pipelang::models {

struct CompositionOrigin {
    std::string module;
    std::string path;
    std::size_t offset = 0;
};

struct CompositionDependency {
    std::string parameter;
    Type type;
    std::size_t target = 0;
    CompositionOrigin origin;
};

enum class CompositionInput { None, Snapshot, Borrowed };

struct CompositionFactoryParameter {
    std::string name;
    Type type;
    std::optional<std::size_t> target;
};

struct CompositionFactory {
    Type receiver;
    std::size_t receiverIndex = 0;
    std::string method;
    std::string identity;
    std::string signature;
    CompositionOrigin origin;
    std::vector<CompositionFactoryParameter> parameters;
};

struct CompositionCleanup {
    Type contract;
    std::string method;
    std::string identity;
    std::string signature;
    CompositionOrigin origin;
    CompositionOrigin selectorOrigin;
};

struct CompositionService {
    Type service;
    Type implementation;
    ServiceLifetime lifetime = ServiceLifetime::Transient;
    CompositionInput input = CompositionInput::None;
    std::string constructorIdentity;
    std::string constructorSignature;
    CompositionOrigin origin;
    CompositionOrigin constructorOrigin;
    std::vector<CompositionDependency> dependencies;
    std::optional<CompositionFactory> factory;
    std::optional<std::size_t> cleanupIndex;
    bool rootResolvable = false;
    bool scopeResolvable = false;
};

// This describes checked construction recipes, not constructed values or caches.
// Order entries name recipes, so a transient may still be constructed repeatedly.
struct CompositionGraph {
    std::string name;
    CompositionOrigin origin;
    std::vector<std::string> includes;
    std::vector<CompositionCleanup> cleanupContracts;
    std::vector<CompositionService> services;
    std::vector<std::size_t> exports;
    std::vector<std::size_t> constructionOrder;
};

nlohmann::json compositionGraphJson(const CompositionGraph& graph);

}
