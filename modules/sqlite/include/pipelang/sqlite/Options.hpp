// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace pipelang::sqlite {
struct Options {
    int busyTimeoutMilliseconds = 100;
    int queryTimeoutMilliseconds = 1000;
    std::int64_t maximumVmSteps = 1000000;
};
}
