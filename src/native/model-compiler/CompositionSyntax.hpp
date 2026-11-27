// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Type.hpp"
#include "CompositionMethodSelector.hpp"
#include <cstddef>
#include <optional>

namespace pipelang::models {

enum class ServiceLifetime { Singleton, Scoped, Transient };

struct CompositionRegistration {
    Type service;
    Type implementation;
    ServiceLifetime lifetime = ServiceLifetime::Transient;
    bool borrowed = false;
    std::size_t offset = 0;
    std::optional<CompositionMethodSelector> factory;
};

struct CompositionInclude {
    std::string name;
    std::size_t offset = 0;
};

struct CompositionDeclaration {
    std::string name;
    std::string module;
    std::string originPath;
    std::size_t offset = 0;
    std::vector<CompositionRegistration> registrations;
    std::vector<CompositionInclude> includes;
    std::vector<Type> exports;
    std::vector<std::size_t> exportOffsets;
    std::string sourceName;
    std::vector<CompositionMethodSelector> cleanupContracts;
};

}
