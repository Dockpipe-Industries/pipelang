// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/model/floating_environment.hpp>

#include <array>
#include <cfenv>
#include <stdexcept>

template<class Invoke>
void checkFloatingObservation(const Invoke& invoke)
{
    namespace facts = pipelang::model::floating_environment;
    struct Expected {
        int rounding;
        std::uint32_t portable;
        std::uint32_t binary32;
    };
    constexpr std::array samples{
        Expected{FE_TONEAREST, facts::portableNearest, facts::binary32Nearest},
        Expected{FE_UPWARD, facts::portableUp, facts::binary32Up},
        Expected{FE_DOWNWARD, facts::portableDown, facts::binary32Down},
        Expected{FE_TOWARDZERO, facts::portableTowardZero, facts::binary32TowardZero},
        Expected{FE_TONEAREST, facts::portableNearest, facts::binary32Nearest}
    };
    struct RestoreNumericEnvironment {
        std::fenv_t environment;

        RestoreNumericEnvironment()
        {
            if (std::fegetenv(&environment) != 0)
                throw std::runtime_error("could not capture the caller's floating environment");
        }

        ~RestoreNumericEnvironment() { std::fesetenv(&environment); }
    } restore;

    constexpr auto portableModes = facts::portableNearest | facts::portableUp |
        facts::portableDown | facts::portableTowardZero;
    constexpr auto binary32Modes = facts::binary32Nearest | facts::binary32Up |
        facts::binary32Down | facts::binary32TowardZero;
    for (const auto& sample : samples) {
        if (std::fesetround(sample.rounding) != 0 || std::feclearexcept(FE_ALL_EXCEPT) != 0 ||
            std::feraiseexcept(FE_INEXACT) != 0)
            throw std::runtime_error("could not change the caller's floating environment");

        const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
        const auto result = invoke();
        if (!result.is_success())
            throw std::runtime_error(result.error());
        const auto observed = static_cast<std::uint32_t>(result.value());
        if ((observed & portableModes) != sample.portable ||
            ((observed & facts::binary32RoundingKnown) != 0 && (observed & binary32Modes) != sample.binary32))
            throw std::runtime_error("source reported the wrong rounding mode");

        if (std::fegetround() != sample.rounding || std::fetestexcept(FE_ALL_EXCEPT) != flags)
            throw std::runtime_error("observation changed the caller's floating environment");
    }
}
