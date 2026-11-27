// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>

namespace lifecycle_test {
struct ResourceEvent {
    std::string operation;
    std::int64_t resource = 0;
    std::string kind;
    std::int64_t context = 0;
    std::int64_t dependency = 0;

    bool operator==(const ResourceEvent&) const = default;
};
}
