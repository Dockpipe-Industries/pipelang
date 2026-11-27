// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "AsyncFinallyLowering.hpp"

#include <utility>

namespace pipelang::models {

AsyncFinallyLowering::Region AsyncFinallyLowering::enter(std::size_t loopDepth) {
    const auto suffix = std::to_string(sequence_++);
    Region region{"pl_exit" + suffix, "pl_cleanup" + suffix, loopDepth};
    regions_.push_back(region);
    return region;
}

AsyncFinallyLowering::Region AsyncFinallyLowering::leave() {
    auto region = std::move(regions_.back());
    regions_.pop_back();
    return region;
}

const AsyncFinallyLowering::Region* AsyncFinallyLowering::returning() noexcept {
    if (regions_.empty())
        return nullptr;
    auto& region = regions_.back();
    region.returns = true;
    return &region;
}

const AsyncFinallyLowering::Region* AsyncFinallyLowering::loopTransfer(
    std::size_t loopDepth, bool continuing) noexcept {
    if (regions_.empty() || regions_.back().loopDepth != loopDepth)
        return nullptr;
    auto& region = regions_.back();
    if (continuing)
        region.continues = true;
    else
        region.breaks = true;
    return &region;
}

}
