// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ServiceTypes.hpp"

namespace lifecycle_test::manual {
struct ScopeCache {
    std::optional<Contracts::IScopedResource> resource;
};
}
