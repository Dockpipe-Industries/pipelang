// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pipelang/model/result.hpp>
#include <atomic>
#include <cstdint>
#include <limits>

namespace pipelang::di::detail {
// Technical ticket identity only; this sequence stores no services or lifetimes.
class ScopeStampSequence {
public:
    explicit ScopeStampSequence(std::uint64_t first = 1) : next_(first) {}
    model::Result<std::uint64_t> next() {
        auto candidate = next_.load(std::memory_order_relaxed);
        for (;;) {
            if (candidate == 0 || candidate == std::numeric_limits<std::uint64_t>::max())
                return model::Result<std::uint64_t>::Failure("di.scope_identity_exhausted");
            if (next_.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
                return model::Result<std::uint64_t>::Success(candidate);
        }
    }
private:
    std::atomic<std::uint64_t> next_;
};
}
