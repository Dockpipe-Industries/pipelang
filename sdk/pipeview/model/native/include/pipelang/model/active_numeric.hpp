// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "numeric.hpp"
#if defined(__linux__) && defined(__x86_64__) && defined(__SSE2__)
#include <emmintrin.h>
#endif

#if !defined(PIPELANG_ACTIVE_HOST_FLOATING) || PIPELANG_ACTIVE_HOST_FLOATING != 1
#error "Active-host numeric exports require the selected strict-floating CMake contract"
#endif
#if !defined(__GNUC__) && !defined(__clang__)
#error "Active-host numeric exports require a qualified GNU-compatible or Clang compiler"
#endif

namespace pipelang::model {
template<class Float>
struct ActiveNumericOperands {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    Float first;
    Float second;
};

template<class Float>
Float activeFloatingValue(Float value) noexcept {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    volatile Float materialized = value;
    return materialized;
}

// Volatile operands retain execution under the caller's controls, including
// literal-only expressions. Each source operation has its own width boundary.
template<class Float>
Float activeAdd(ActiveNumericOperands<Float> operands) noexcept {
    volatile Float first = operands.first;
    volatile Float second = operands.second;
    volatile Float result = first + second;
    return result;
}

template<class Float>
Float activeSubtract(ActiveNumericOperands<Float> operands) noexcept {
    volatile Float first = operands.first;
    volatile Float second = operands.second;
    volatile Float result = first - second;
    return result;
}

template<class Float>
Float activeMultiply(ActiveNumericOperands<Float> operands) noexcept {
    volatile Float first = operands.first;
    volatile Float second = operands.second;
    volatile Float result = first * second;
    return result;
}

template<class Float>
Float activeDivide(ActiveNumericOperands<Float> operands) noexcept {
    volatile Float first = operands.first;
    volatile Float second = operands.second;
    volatile Float result = first / second;
    return result;
}

template<class Float>
Float activeFromInteger(std::int64_t value) noexcept {
    static_assert(std::is_same_v<Float, float> || std::is_same_v<Float, double>);
    volatile std::int64_t input = value;
    volatile Float result = static_cast<Float>(input);
    return result;
}

// This selected operation is qualified for Linux x86-64 SSE2. The intrinsic
// defines overflow, underflow and directed rounding without a C++ out-of-range
// floating cast. Other active operations remain available on their own profiles.
template<class Platform = void>
float activeNarrowFloat([[maybe_unused]] double value) noexcept
{
#if defined(__linux__) && defined(__x86_64__) && defined(__SSE2__)
    volatile double input = value;
    volatile float converted = _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), _mm_set_sd(input)));
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(converted));
    if ((bits & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000))
        return std::bit_cast<float>((bits & UINT32_C(0x80000000)) | UINT32_C(0x7fc00000));
    return converted;
#else
    static_assert(!std::is_same_v<Platform, Platform>,
        "Active float32.FromFloat requires the qualified Linux x86-64 SSE2 profile");
    return 0.0f;
#endif
}

}
