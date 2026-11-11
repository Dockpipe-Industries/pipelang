// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RootCache.hpp"
#include "ScopeCache.hpp"

namespace lifecycle_test::manual {
struct Stage {
    RootCache root;
    ScopeCache scope;
};
}
