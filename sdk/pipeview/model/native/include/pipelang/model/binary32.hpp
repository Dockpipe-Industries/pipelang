// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "numeric.hpp"
#include "result.hpp"

#include <cfenv>
#include <cfloat>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

static_assert(CHAR_BIT == 8 && sizeof(float) == 4 && sizeof(std::uint32_t) == 4,
    "PipeLang requires 32-bit float and uint32_t");
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::radix == 2
    && std::numeric_limits<float>::digits == 24 && std::numeric_limits<float>::min_exponent == -125
    && std::numeric_limits<float>::max_exponent == 128,
    "PipeLang requires IEEE-754 binary32 float");

static_assert(FLT_EVAL_METHOD == 0, "PipeLang float32 requires nominal-width floating evaluation");

namespace pipelang::model {

// The owner checks the native thread environment before source evaluation.
// Check native control registers where their semantics are known; otherwise
// probe gradual subnormal production and consumption without changing modes.
inline void requireBinary32Environment() {
    if (std::fegetround() != FE_TONEAREST)
        throw ExecutionFailure("float32 requires round-to-nearest floating mode");
#if defined(__SSE__) || defined(_M_X64)
    // Native hosts can change SSE rounding separately from the x87 control word
    // observed by some fenv implementations.
    const unsigned control = _mm_getcsr();
    if ((control & 0x6000U) != 0)
        throw ExecutionFailure("float32 requires round-to-nearest SSE mode");
    // FTZ (bit 15) and DAZ (bit 6) fully identify non-IEEE subnormal handling
    // for these SSE operations. Reading them avoids executing subnormal probes
    // on every source invocation. The guard never writes the control register.
    if ((control & 0x8040U) != 0)
        throw ExecutionFailure("float32 requires gradual subnormal arithmetic");
#else
    volatile float minimum = std::numeric_limits<float>::min();
    volatile float subnormal = std::numeric_limits<float>::denorm_min();
    volatile float half = 0.5f;
    volatile float two = 2.0f;
    const float produced = minimum * half;
    const float consumed = subnormal * two;
    if (std::bit_cast<std::uint32_t>(produced) != UINT32_C(0x00400000) ||
        std::bit_cast<std::uint32_t>(consumed) != UINT32_C(0x00000002))
        throw ExecutionFailure("float32 requires gradual subnormal arithmetic");
#endif
}

}
