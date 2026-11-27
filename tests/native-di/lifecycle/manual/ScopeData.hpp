// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RootData.hpp"
#include "ScopeCache.hpp"

namespace lifecycle_test::manual {
struct ScopeData {
    std::shared_ptr<RootData> root;
    std::shared_ptr<Context> execution;
    Contracts::ScopeStamp stamp;
    ScopeCache cache;
};
}
