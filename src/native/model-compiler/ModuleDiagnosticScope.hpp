// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <utility>

namespace pipelang::models {
// Private companion for normalizing a generic body owned by another module.
class ModuleDiagnosticScope final {
public:
    ModuleDiagnosticScope(std::string& current, const std::string& module)
        : current_(current), previous_(current)
    {
        current_ = module;
    }
    ~ModuleDiagnosticScope()
    {
        current_ = std::move(previous_);
    }
    ModuleDiagnosticScope(const ModuleDiagnosticScope&) = delete;
    ModuleDiagnosticScope& operator=(const ModuleDiagnosticScope&) = delete;
private:
    std::string& current_;
    std::string previous_;
};
}
