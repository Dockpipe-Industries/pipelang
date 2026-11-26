// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <xmmintrin.h>

namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_active_narrowing;

namespace {

struct FloatingEnvironment {
    std::fenv_t environment;
    unsigned control = _mm_getcsr();

    FloatingEnvironment() { std::fegetenv(&environment); }

    ~FloatingEnvironment() { restore(); }

    void restore() const
    {
        std::fesetenv(&environment);
        _mm_setcsr(control);
    }
};

struct Narrowed {
    std::uint32_t bits;
    int exceptions;
    int rounding;
    unsigned control;
};

Narrowed observe(float value)
{
    return {std::bit_cast<std::uint32_t>(value), std::fetestexcept(FE_ALL_EXCEPT), std::fegetround(), _mm_getcsr()};
}

float narrowHardware(double value)
{
    volatile double input = value;
    volatile float converted = _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), _mm_set_sd(input)));
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(converted));
    constexpr std::uint32_t sign = 0x80000000U;
    constexpr std::uint32_t infinity = 0x7f800000U;
    constexpr std::uint32_t quietNaN = 0x7fc00000U;
    if ((bits & ~sign) > infinity)
        return std::bit_cast<float>((bits & sign) | quietNaN);
    return converted;
}

bool verify(const native::HostedInstance& instance, pipelang::model::TaskScope& scope, std::uint64_t word)
{
    const auto input = std::bit_cast<double>(word);
    const std::array<native::HostedCallArgument, 1> args{native::HostedValue::fromFloat(input)};
    const FloatingEnvironment initial;
    const auto expected = observe(narrowHardware(input));

    initial.restore();
    const auto result = instance.startWithValues(scope, args).result();
    if (!result.is_success()) {
        std::cerr << result.error() << '\n';
        return false;
    }

    const auto actual = observe(result.value().float32());
    if (actual.bits == expected.bits && actual.exceptions == expected.exceptions &&
        actual.rounding == expected.rounding && actual.control == expected.control)
        return true;

    std::cerr << "Active-host narrowing mismatch for 0x" << std::hex << word
              << " at rounding mode " << std::dec << expected.rounding << '\n';
    return false;
}

}

int main()
{
    const FloatingEnvironment initial;
    const auto session = native::HostedInstance::bind(proof_active_narrowing, {});
    if (!session.is_success()) {
        std::cerr << session.error() << '\n';
        return 1;
    }

    constexpr std::array<std::uint64_t, 18> inputs{
        0x0000000000000000ULL, 0x8000000000000000ULL,
        0x0000000000000001ULL, 0x8000000000000001ULL,
        0x3690000000000000ULL, 0xb690000000000000ULL,
        0x3ff0000010000000ULL, 0xbff0000010000000ULL,
        0x47efffffe0000000ULL, 0xc7efffffe0000000ULL,
        0x47effffff0000000ULL, 0xc7effffff0000000ULL,
        0x7fefffffffffffffULL, 0xffefffffffffffffULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL,
        0x7ff0123456789abcULL, 0xfff8123456789abcULL,
    };
    constexpr unsigned flushMask = (1U << 15) | (1U << 6);
    pipelang::model::TaskScope scope;
    for (const auto rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        if (std::fesetround(rounding) != 0)
            return 2;

        for (const unsigned flush : {0U, flushMask}) {
            _mm_setcsr((_mm_getcsr() & ~flushMask) | flush);
            for (const auto word : inputs) {
                std::feclearexcept(FE_ALL_EXCEPT);
                std::feraiseexcept(FE_DIVBYZERO);
                if (!verify(session.value(), scope, word))
                    return 3;
            }
        }
    }

    std::cout << "Native active-host narrowing preserved result bits, rounding controls and exception flags\n";
}
