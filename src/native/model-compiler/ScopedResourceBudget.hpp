// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceBudget.hpp"
#include <string>

namespace pipelang::models {
// Scoped per-thread context follows DiagnosticOriginScope. Own the policy so a
// temporary caller value remains valid; nesting restores the previous context.
class ScopedResourceBudget final {
    ResourceBudget budget_;
    std::string sourcePath_;
    std::string sourceHash_;
    const ScopedResourceBudget* previous_;
public:
    explicit ScopedResourceBudget(ResourceBudget budget, std::string sourcePath = {}, std::string sourceHash = {});
    ~ScopedResourceBudget();
    ScopedResourceBudget(const ScopedResourceBudget&) = delete;
    ScopedResourceBudget& operator=(const ScopedResourceBudget&) = delete;
    const ResourceBudget& budget() const noexcept { return budget_; }
    const std::string& sourcePath() const noexcept { return sourcePath_; }
    const std::string& sourceHash() const noexcept { return sourceHash_; }
};
std::string resourceBudgetSourcePath();
std::string resourceBudgetSourceHash();
}
