// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "UInt32IntegerOnly.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

int main() {
    uint32_only::IntegerWords model;
    static_assert(std::is_same_v<decltype(model.Read()), std::uint32_t>);
    static_assert(std::is_same_v<decltype(model.Widen(0)), std::int64_t>);
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    assert(model.Read() == maximum);
    assert(model.Widen(maximum) == INT64_C(4294967295));
    assert(model.Narrow(INT64_C(4294967295)) == maximum);
    assert(model.Mix(1) == UINT32_C(8193));
    assert(model.Nested() == UINT32_C(0x80000000));
    assert(model.Keys() == 16);
    assert(model.SharedKeys().value());
}
