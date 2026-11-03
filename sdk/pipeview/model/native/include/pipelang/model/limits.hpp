// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"
#include <cstddef>

namespace pipelang::model {
inline constexpr std::size_t defaultEvaluationSteps = 65536;
inline constexpr std::size_t maximumInitializationSteps = 1048576;

inline void requireInitializationSteps(std::size_t requested) {
    if (requested == 0 || requested > maximumInitializationSteps)
        throw ExecutionFailure("initialization steps must be between 1 and 1048576");
}
}
