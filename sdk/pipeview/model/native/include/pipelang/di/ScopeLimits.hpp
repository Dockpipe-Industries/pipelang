// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

namespace pipelang::di {
struct ScopeLimits {
    // Bookkeeping remains bounded by the existing object domain's 4,096 cells.
    // These are ledger limits, not a claim of object-domain integration.
    static constexpr std::size_t maximumEntries = 4096;
    std::size_t activities = 128;
    std::size_t ownedRecords = 128;
};
}
