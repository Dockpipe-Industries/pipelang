// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace pipelang::model {

// The language uses signed64 integers. These familiar signed32 sentinels leave
// ordinary exponent arithmetic representable without narrowing native integers.
template<class Float>
std::int64_t integerBinaryExponent(Float value) noexcept
{
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    volatile Float input = value;
    const int exponent = std::ilogb(input);
    using Bits = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;
    constexpr Bits sign = Bits{1} << (sizeof(Bits) * CHAR_BIT - 1);
    constexpr Bits infinity = std::is_same_v<Float, float>
        ? static_cast<Bits>(UINT32_C(0x7f800000)) : static_cast<Bits>(UINT64_C(0x7ff0000000000000));
    const Bits magnitude = std::bit_cast<Bits>(value) & ~sign;
    // Representation checks must not turn DAZ-treated subnormals into zero.
    // Execute ilogb first so invalid inputs retain the native status effects.
    if (magnitude == 0 || magnitude > infinity)
        return INT64_C(-2147483648);
    if (magnitude == infinity)
        return INT64_C(2147483647);
    return exponent;
}

template<class Float>
Float scaleBinaryExponent(Float value, std::int64_t exponent) noexcept
{
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    int nativeExponent;
    if (exponent < INT_MIN) {
        nativeExponent = INT_MIN;
    } else if (exponent > INT_MAX) {
        nativeExponent = INT_MAX;
    } else {
        nativeExponent = static_cast<int>(exponent);
    }
    volatile Float input = value;
    volatile Float result = std::ldexp(input, nativeExponent);
    return result;
}

}
