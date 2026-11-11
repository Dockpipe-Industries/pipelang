// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <cstddef>
#include <string_view>

namespace pipelang::models {
// Admission policy, not source representation or a language size restriction.
// Source bytes remain separate dynamically owned units. Transport/index accounting
// bounds their storage; no independent per-file or closure byte ceiling exists.
struct ResourceBudget {
    std::size_t inputBytes = 4U << 20;
    std::size_t outputBytes = 4U << 20;
    std::size_t indexedBytes = 16U << 20;
    std::size_t indexedFiles = 4096;
    std::size_t directoryEntries = 32768;
    std::size_t directoryDepth = 32;
    std::size_t sourceFragments = 4096;
    std::size_t modules = 128;
    std::size_t declarations = 128;
    std::size_t tokens = 32768;
    std::size_t expressions = 16384;
    std::size_t genericInstances = 128;
    std::size_t implementationUnits = 256;

    static ResourceBudget fromJson(const nlohmann::json& value);
    static ResourceBudget fromText(std::string_view value);
    nlohmann::json json() const;
    bool operator==(const ResourceBudget&) const = default;
};
const ResourceBudget& resourceBudget() noexcept;
bool budgetExceeded(std::size_t used, std::size_t additional, std::size_t limit) noexcept;
}
