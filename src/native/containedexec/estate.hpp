// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../support/support.hpp"

namespace pipelang::contained {
// Internal allocation sampler shared by the Linux guard and its independent
// filesystem probes. A changing tree must yield a complete bounded traversal.
std::uint64_t allocatedEstate(const std::filesystem::path& estate, std::uint64_t limit);
}
