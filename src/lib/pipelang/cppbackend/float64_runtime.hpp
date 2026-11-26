#ifndef PIPELANG_FLOAT64_COMPARISON_RUNTIME_V1
#define PIPELANG_FLOAT64_COMPARISON_RUNTIME_V1

#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0) || defined(_M_FP_FAST)
#error "PipeLang binary64 comparisons require strict IEEE floating-point compilation"
#endif

static_assert(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(std::uint64_t) == 8,
    "PipeLang binary64 comparisons require 64-bit double and uint64_t");
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2
    && std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::min_exponent == -1021
    && std::numeric_limits<double>::max_exponent == 1024,
    "PipeLang binary64 comparisons require IEEE-754 binary64 double");

namespace pipelang::float64_comparison_detail {
inline double from_bits(std::uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}

#endif
