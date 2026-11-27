// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ServiceTypes.hpp"

namespace lifecycle_test::manual {
struct RootCache {
    std::optional<Factories::RootDependencyFactory> dependencyFactory;
    std::optional<Factories::RootFactory> rootFactory;
    std::optional<Factories::ScopedFactory> scopedFactory;
    std::optional<Factories::TransientFactory> transientFactory;
    std::optional<Contracts::IRootResource> resource;
};
}
