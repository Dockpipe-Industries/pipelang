// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ScopePhase.hpp"
#include <cstddef>

namespace pipelang::di {
// A failed/cancelled attempt has an unknown external effect and is never replayed.
struct CleanupStatus {
    ScopePhase phase = ScopePhase::Open;
    std::size_t pending = 0;
    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    std::size_t unknown = 0;
};
}
