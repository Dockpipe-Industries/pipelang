// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_hosted_binary32;

namespace {

class SavedEnvironment {
public:
    SavedEnvironment()
    {
        if (std::fegetenv(&environment_) != 0)
            throw std::runtime_error("could not save the floating-point environment");
    }

    ~SavedEnvironment()
    {
        std::fesetenv(&environment_);
    }

private:
    std::fenv_t environment_;
};

void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

void checkLiteral(const native::HostedValueResult& value)
{
    if (!value.is_success())
        throw std::runtime_error(value.error());
    require(std::bit_cast<std::uint32_t>(value.value().float32()) == UINT32_C(0x3dcccccd),
            "native binary32 literal changed");
}

}

int main(int argc, char** argv)
{
    try {
        const bool activeHost = argc == 2 && std::string_view(argv[1]) == "--active-host";
        const SavedEnvironment environment;
        auto binding = native::HostedInstance::bind(proof_hosted_binary32, {});
        if (!binding.is_success())
            throw std::runtime_error(binding.error());
        model::TaskScope tasks;
        const auto literal = [&] { return binding.value().startValue(tasks, {}).result(); };
        checkLiteral(literal());

        require(std::fesetround(FE_DOWNWARD) == 0, "could not set the downward rounding mode");
        const auto rounded = literal();
        const auto initialized = native::HostedInstance::bind(proof_hosted_binary32, {});
        require(std::fegetround() == FE_DOWNWARD, "native invocation changed the caller's rounding mode");
        require(std::fesetround(FE_TONEAREST) == 0, "could not restore round-to-nearest");

        if (activeHost) {
            checkLiteral(rounded);
            require(initialized.is_success(), "explicit active-host construction rejected the caller's mode");
        } else {
            constexpr auto expected = "float32 requires round-to-nearest floating mode";
            require(rounded.is_failure() && rounded.error() == expected, "portable binary32 invocation admitted downward rounding");
            require(initialized.is_failure() && initialized.error() == expected, "portable binary32 construction admitted downward rounding");
        }

#if defined(__SSE__) || defined(_M_X64)
        const unsigned control = _mm_getcsr();
        for (unsigned state : {0x8000U, 0x0040U, 0x2000U, 0x4000U, 0x6000U}) {
            _mm_setcsr(control | state);
            const auto changed = literal();
            require(_mm_getcsr() == (control | state), "native invocation changed the caller's SSE controls");
            _mm_setcsr(control);

            if (activeHost)
                checkLiteral(changed);
            else
                require(changed.is_failure(), "portable binary32 invocation admitted a non-portable SSE mode");
        }
#endif
        checkLiteral(literal());
        std::cout << "Hosted binary32 environment contract passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
