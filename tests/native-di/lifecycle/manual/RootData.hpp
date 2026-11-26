// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RootCache.hpp"

namespace lifecycle_test::manual {
struct RootData {
    std::shared_ptr<pipelang::di::ExecutionOwner> owner;
    std::shared_ptr<Context> execution;
    std::shared_ptr<ResourcePortAdapter> port;
    RootCache cache;
};
}
