// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ScopedResourceBudget.hpp"
#include <utility>

namespace pipelang::models {
namespace {
thread_local const ScopedResourceBudget* active = nullptr;
const ResourceBudget defaults;
}
ScopedResourceBudget::ScopedResourceBudget(ResourceBudget budget, std::string sourcePath, std::string sourceHash)
    : budget_(std::move(budget)), sourcePath_(std::move(sourcePath)), sourceHash_(std::move(sourceHash)), previous_(active) {
    // The public C++ value type cannot bypass JSON profile validation with zero
    // counters. Native callers and serialized callers share one admission rule.
    budget_ = ResourceBudget::fromJson(budget_.json());
    active = this;
}
ScopedResourceBudget::~ScopedResourceBudget() {
    active = previous_;
}
const ResourceBudget& resourceBudget() noexcept {
    return active ? active->budget() : defaults;
}
std::string resourceBudgetSourcePath() {
    return active ? active->sourcePath() : std::string{};
}
std::string resourceBudgetSourceHash() {
    return active ? active->sourceHash() : std::string{};
}
}
