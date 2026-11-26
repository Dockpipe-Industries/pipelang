// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace pipelang::di {
class ScopeState;

// Opaque progress identity plus compiler-owned typed-orchestration keys.
// This contains neither an instance nor a callable/provider lookup.
class CleanupWork {
public:
    CleanupWork() = default;
    std::uint64_t key() const noexcept { return key_; }
    std::uint64_t constructionAttempt() const noexcept { return attempt_; }
    bool operator==(const CleanupWork&) const = default;
private:
    friend class ScopeState;
    CleanupWork(std::uint64_t scope, std::uint64_t serial,
                std::uint64_t key, std::uint64_t attempt)
        : scope_(scope), serial_(serial), key_(key), attempt_(attempt) {}
    std::uint64_t scope_ = 0;
    std::uint64_t serial_ = 0;
    std::uint64_t key_ = 0;
    std::uint64_t attempt_ = 0;
};
}
