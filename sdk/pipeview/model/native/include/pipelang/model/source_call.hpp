// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"

#include <cstddef>

namespace pipelang::model {

// A value describes one source ancestry, not the number of active tasks.
// Children and parallel workers inherit it; siblings never mutate one another.
// Keeping it independent of evaluation budgets also preserves ancestry when a
// genuine host suspension starts a fresh work segment.
class SourceCallContext {
public:
    static constexpr std::size_t maximumDepth = 32;

    [[nodiscard]] SourceCallContext enter() const {
        requireCapacity(depth_);
        return SourceCallContext(depth_ + 1);
    }

    static void requireCapacity(std::size_t parentDepth) {
        if (parentDepth >= maximumDepth)
            throw EvaluationLimitFailure("source call depth limit exceeded");
    }

    std::size_t depth() const noexcept {
        return depth_;
    }

    SourceCallContext() = default;

private:
    explicit SourceCallContext(std::size_t depth) : depth_(depth) {
    }

    std::size_t depth_ = 0;
};

}
