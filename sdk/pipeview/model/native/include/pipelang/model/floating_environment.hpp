// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cfenv>
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace pipelang::model {
namespace floating_environment {
// Language-level facts, independent of native register layouts. A positive mode
// fact is set only when the corresponding observation is known.
inline constexpr std::uint32_t portableRoundingKnown = 1U;
inline constexpr std::uint32_t portableNearest = 2U;
inline constexpr std::uint32_t portableDown = 4U;
inline constexpr std::uint32_t portableUp = 8U;
inline constexpr std::uint32_t portableTowardZero = 16U;
inline constexpr std::uint32_t binary32RoundingKnown = 32U;
inline constexpr std::uint32_t binary32Nearest = 64U;
inline constexpr std::uint32_t binary32Down = 128U;
inline constexpr std::uint32_t binary32Up = 256U;
inline constexpr std::uint32_t binary32TowardZero = 512U;
inline constexpr std::uint32_t binary32InputsKnown = 1024U;
inline constexpr std::uint32_t binary32GradualInputs = 2048U;
inline constexpr std::uint32_t binary32ResultsKnown = 4096U;
inline constexpr std::uint32_t binary32GradualResults = 8192U;
}

namespace floating_environment_detail {
inline int portableRoundingMode() noexcept
{
    return std::fegetround();
}

#if defined(__SSE__) || defined(_M_X64)
inline unsigned binary32ControlWord() noexcept
{
    return _mm_getcsr();
}
#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
inline std::uint64_t aarch64ControlWord() noexcept
{
    std::uint64_t control;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control) : : "memory");
    return control;
}
#endif

// Arm FPCR: RMode[23:22], FZ[24], AH[1], FIZ[0]. AH and FIZ are
// RES0 without FEAT_AFP. With AH enabled, some instructions override rounding
// and denormal handling, so this profile leaves all binary32 facts unknown.
// Arm's FPCR register definition and alternate-handling description:
// https://support.arm.com/documentation/102807/0001/AArch64-System-registers/AArch64-Generic-System-Control-registers-summary/FPCR--Floating-point-Control-Register
// https://documentation-service.arm.com/static/6526e1bd9e189a266cef8412
inline std::uint32_t aarch64Binary32Facts(std::uint64_t control) noexcept
{
    namespace facts = floating_environment;
    constexpr std::uint64_t flushInputs = std::uint64_t{1} << 0;
    constexpr std::uint64_t alternateHandling = std::uint64_t{1} << 1;
    constexpr std::uint64_t roundingMask = std::uint64_t{3} << 22;
    constexpr std::uint64_t flushResults = std::uint64_t{1} << 24;
    if ((control & alternateHandling) != 0)
        return 0;

    std::uint32_t result = facts::binary32RoundingKnown
        | facts::binary32InputsKnown | facts::binary32ResultsKnown;
    switch (control & roundingMask) {
    case 0:
        result |= facts::binary32Nearest;
        break;
    case std::uint64_t{1} << 22:
        result |= facts::binary32Up;
        break;
    case std::uint64_t{2} << 22:
        result |= facts::binary32Down;
        break;
    case std::uint64_t{3} << 22:
        result |= facts::binary32TowardZero;
        break;
    }
    if ((control & (flushInputs | flushResults)) == 0)
        result |= facts::binary32GradualInputs;
    if ((control & flushResults) == 0)
        result |= facts::binary32GradualResults;
    return result;
}
}

// A synchronous observation of this thread on every invocation. No arithmetic,
// probes, flag clearing, environment restoration, or control writes occur here.
// SSE and ordinary (non-alternate-handling) AArch64 control modes are decoded.
// Other architectures report binary32 control facts as unknown; the existing
// ordinary binary32 runtime guard retains its separate probe policy.
inline std::uint32_t observeFloatingEnvironment() noexcept
{
    namespace facts = floating_environment;
    std::uint32_t result = 0;
    const int portable = floating_environment_detail::portableRoundingMode();
    if (portable == FE_TONEAREST)
        result |= facts::portableRoundingKnown | facts::portableNearest;
#ifdef FE_DOWNWARD
    else if (portable == FE_DOWNWARD)
        result |= facts::portableRoundingKnown | facts::portableDown;
#endif
#ifdef FE_UPWARD
    else if (portable == FE_UPWARD)
        result |= facts::portableRoundingKnown | facts::portableUp;
#endif
#ifdef FE_TOWARDZERO
    else if (portable == FE_TOWARDZERO)
        result |= facts::portableRoundingKnown | facts::portableTowardZero;
#endif
#if defined(__SSE__) || defined(_M_X64)
    const unsigned control = floating_environment_detail::binary32ControlWord();
    result |= facts::binary32RoundingKnown | facts::binary32InputsKnown | facts::binary32ResultsKnown;
    switch (control & 0x6000U) {
    case 0:
        result |= facts::binary32Nearest;
        break;
    case 0x2000U:
        result |= facts::binary32Down;
        break;
    case 0x4000U:
        result |= facts::binary32Up;
        break;
    case 0x6000U:
        result |= facts::binary32TowardZero;
        break;
    }
    if ((control & 0x0040U) == 0)
        result |= facts::binary32GradualInputs;
    if ((control & 0x8000U) == 0)
        result |= facts::binary32GradualResults;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    result |= floating_environment_detail::aarch64Binary32Facts(
        floating_environment_detail::aarch64ControlWord());
#endif
    return result;
}
}
