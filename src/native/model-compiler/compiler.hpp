// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../third_party/nlohmann/json.hpp"
#include "ResourceBudget.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace pipelang::models {

inline constexpr std::size_t sourcePathLimit = 64 * 1024;
inline constexpr std::size_t messageLimit = 4 * 1024 * 1024;

struct Diagnostic : std::runtime_error {
    Diagnostic(std::string message, std::size_t offset)
        : std::runtime_error(std::move(message)), offset(offset) {}
    std::size_t offset;
    std::string module;
    std::string path = {};
};

struct ModuleSource {
    std::string name;
    std::string source;
    std::string path = {};
};

// Compile-time semantic queries use the same closed project linker and checker.
// Ranges are UTF-8 byte offsets in each caller-supplied source fragment.
nlohmann::json formatSource(const std::string& source, unsigned tabSize, bool insertSpaces,
                            std::size_t rangeStart, std::size_t rangeEnd);

nlohmann::json indexProjectSources(const std::vector<ModuleSource>& sources);

nlohmann::json analyzeProjectModules(const std::vector<ModuleSource>& modules, const std::string& entry);

nlohmann::json compile(const std::string& source, const std::string& nameSpace);
nlohmann::json compileModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                              const std::string& nameSpace);

nlohmann::json compileObjects(const std::string& source, const std::vector<std::string>& models,
                              const std::string& nameSpace);
nlohmann::json compileModuleObjects(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::vector<std::string>& models, const std::string& nameSpace);

nlohmann::json compileProjectModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::string& nameSpace, bool activeHostNumericEnvironment = false);

nlohmann::json compileProjectImplementation(const std::vector<ModuleSource>& modules,
                                           const std::string& entry, const std::string& nameSpace,
                                           const std::string& emission, bool activeHostNumericEnvironment = false);

nlohmann::json compileProjectCompositionFactory(const std::vector<ModuleSource>& modules,
                                                const std::string& entry, const std::string& nameSpace,
                                                const std::string& emission = {},
                                                bool activeHostNumericEnvironment = false);

}
