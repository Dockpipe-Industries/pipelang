// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pipelang::models {

// Emission-time protected-region stack. Transfers leave their lexical scopes,
// visit each intervening cleanup once, then resume the original return/loop exit.
// No additional source activation or coroutine is introduced by this lowering.
class AsyncFinallyLowering {
public:
    struct Region {
        std::string exit;
        std::string label;
        std::size_t loopDepth = 0;
        bool breaks = false;
        bool continues = false;
        bool returns = false;
    };

    Region enter(std::size_t loopDepth);
    Region leave();
    const Region* returning() noexcept;
    const Region* loopTransfer(std::size_t loopDepth, bool continuing) noexcept;

private:
    std::vector<Region> regions_;
    std::size_t sequence_ = 0;
};

}
