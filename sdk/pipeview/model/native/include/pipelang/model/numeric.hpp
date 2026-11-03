// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0) || defined(_M_FP_FAST)
#error "PipeLang binary64 values require strict IEEE floating-point compilation"
#endif

static_assert(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(std::uint64_t) == 8,
    "PipeLang requires 64-bit double and uint64_t");
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2
    && std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::min_exponent == -1021
    && std::numeric_limits<double>::max_exponent == 1024,
    "PipeLang requires IEEE-754 binary64 double");

namespace pipelang::model {

template<class Float>
std::int64_t truncateToInt(Float value) {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    // Both limits are exact powers of two in either floating type. Comparing
    // against rounded INT64_MAX would accidentally admit the unrepresentable
    // positive endpoint. Cast only after proving the truncated value is in range.
    if (!std::isfinite(value) || value < -0x1p63 || value >= 0x1p63)
        throw ExecutionFailure("floating-to-int conversion requires a finite value in signed64 range");
    return static_cast<std::int64_t>(value);
}

template<class Float>
Float roundInteger(std::int64_t value) {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    volatile Float rounded = static_cast<Float>(value);
    return rounded;
}

template<class Float>
std::uint32_t truncateToUInt32(Float value) {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    if (!std::isfinite(value))
        throw ExecutionFailure("floating-to-uint32 conversion requires a finite truncated value in range 0..4294967295");
    const Float truncated = std::trunc(value);
    if (truncated < Float(0) || truncated >= 0x1p32)
        throw ExecutionFailure("floating-to-uint32 conversion requires a finite truncated value in range 0..4294967295");
    return static_cast<std::uint32_t>(truncated);
}

template<class Float>
Float roundUInt32(std::uint32_t value) {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    volatile Float rounded = static_cast<Float>(value);
    return rounded;
}

inline double widenFloat(float value) {

    if (std::isnan(value))
        return std::copysign(std::numeric_limits<double>::quiet_NaN(), std::signbit(value) ? -1.0 : 1.0);
    return static_cast<double>(value);
}

inline float narrowFloat(double value) {
    if (std::isnan(value))
        return std::copysign(std::numeric_limits<float>::quiet_NaN(), std::signbit(value) ? -1.0f : 1.0f);
    if (std::isinf(value))
        return std::copysign(std::numeric_limits<float>::infinity(), std::signbit(value) ? -1.0f : 1.0f);
    const double magnitude = std::abs(value);
    const double maximum = std::numeric_limits<float>::max();
    if (magnitude > maximum) {
        // Casting an out-of-range finite double directly is not portable C++.
        // IEEE nearest/even rounds the upper half-ULP to infinity, including
        // its exact midpoint; smaller excess values round to maximum finite.
        constexpr double overflowMidpoint = 0x1.ffffffp127;
        float rounded = std::numeric_limits<float>::max();
        if (magnitude >= overflowMidpoint)
            rounded = std::numeric_limits<float>::infinity();
        return std::copysign(rounded, std::signbit(value) ? -1.0f : 1.0f);
    }
    // In-range conversion preserves signed zero and gradual subnormals. Tiny
    // finite values may round to signed zero; that underflow is an IEEE value.
    volatile float rounded = static_cast<float>(value);
    return rounded;
}

}
