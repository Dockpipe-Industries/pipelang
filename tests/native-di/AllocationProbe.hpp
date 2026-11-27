// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
namespace di_tests {
struct AllocationCounts { std::size_t calls = 0; std::size_t requestedBytes = 0; };
void BeginAllocationProbe();
AllocationCounts EndAllocationProbe();
}
