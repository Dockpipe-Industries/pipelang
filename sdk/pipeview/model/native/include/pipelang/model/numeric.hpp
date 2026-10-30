// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

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
