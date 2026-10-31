// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../third_party/nlohmann/json.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace pipelang::models {

inline constexpr std::size_t sourceLimit = 64 * 1024;
inline constexpr std::size_t messageLimit = 4 * 1024 * 1024;

struct Diagnostic : std::runtime_error {
    Diagnostic(std::string message, std::size_t offset)
        : std::runtime_error(std::move(message)), offset(offset) {}
    std::size_t offset;
    std::string module;
};

struct ModuleSource {
    std::string name;
    std::string source;
};

nlohmann::json compile(const std::string& source, const std::string& nameSpace);
nlohmann::json compileModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                              const std::string& nameSpace);

nlohmann::json compileObjects(const std::string& source, const std::vector<std::string>& models,
                              const std::string& nameSpace);
nlohmann::json compileModuleObjects(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::vector<std::string>& models, const std::string& nameSpace);

}
