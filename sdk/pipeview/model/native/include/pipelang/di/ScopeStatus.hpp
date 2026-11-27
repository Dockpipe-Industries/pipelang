// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ScopePhase.hpp"
#include <cstddef>

namespace pipelang::di {
struct ScopeStatus {
    ScopePhase phase = ScopePhase::Open;
    std::size_t constructions = 0;
    std::size_t operations = 0;
    std::size_t children = 0;
    std::size_t ownedRemaining = 0;
    std::size_t cleanupCompleted = 0;
    std::size_t cleanupFailures = 0;
    std::size_t cleanupCancellations = 0;
    bool cleanupInProgress = false;
};
}
