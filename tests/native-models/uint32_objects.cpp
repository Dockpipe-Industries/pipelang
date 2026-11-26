// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "UInt32Objects.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    uint32_objects::App model(maximum);
    assert(model.Read() == maximum);
    assert(model.Set(UINT32_C(0x80000000)).value() == UINT32_C(0x80000000));
    assert(model.Rollback(maximum).is_failure());
    assert(model.Read() == UINT32_C(0x80000000));
    assert(model.Set(1).value() == 1);
}
