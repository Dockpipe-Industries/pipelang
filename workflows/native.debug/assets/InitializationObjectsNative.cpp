// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_initialization_objects;

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

auto bind(bool createSecond, std::size_t steps)
{
    const std::array<HostedConstructorArgument, 1> arguments{HostedValue::fromBoolean(createSecond)};
    return HostedInstance::bind(proof_initialization_objects, arguments, steps);
}

void verifyBoundModel(const HostedInstance& instance, bool hasSecond)
{
    pipelang::model::TaskScope scope;
    const auto first = instance.start(scope, {}).result();
    require(first.is_success() && first.value() == 9000, "root receiver constructor value differs");
    const auto second = instance.startMethod(scope, "HasSecond", {}).result();
    require(second.is_success() && second.value().boolean() == hasSecond, "nested optional receiver differs");
}

int main()
{
    try {
        const auto invalid = bind(false, 0);
        require(invalid.is_failure() && invalid.error() == "initialization steps must be between 1 and 1048576",
                "invalid budget was admitted");

        const auto defaultLimit = bind(false, 65536);
        require(defaultLimit.is_failure() && defaultLimit.error() == "source evaluation budget exceeded",
                defaultLimit.is_success() ? "hosted initialization ran beyond the default budget" : defaultLimit.error());
        const auto firstOnly = bind(false, 131072);
        require(firstOnly.is_success(), firstOnly.is_success() ? "" : firstOnly.error());
        verifyBoundModel(firstOnly.value(), false);

        const auto nestedLimit = bind(true, 131072);
        require(nestedLimit.is_failure() && nestedLimit.error() == "source evaluation budget exceeded",
                nestedLimit.is_success() ? "nested constructor reset the initialization budget" : nestedLimit.error());
        const auto complete = bind(true, 262144);
        require(complete.is_success(), complete.is_success() ? "" : complete.error());
        verifyBoundModel(complete.value(), true);

        // Compare the exact budget transitions measured in the C++ fixture.
        for (const bool nested : {false, true}) {
            const std::size_t minimum = nested ? 144031 : 72015;
            const auto below = bind(nested, minimum - 1);
            require(below.is_failure() && below.error() == "source evaluation budget exceeded",
                    "constructor admitted one step below the reference threshold");
            const auto atLimit = bind(nested, minimum);
            require(atLimit.is_success(), atLimit.is_success() ? "" : atLimit.error());
            verifyBoundModel(atLimit.value(), nested);
        }

        std::cout << "Initialization loops share one checked evaluation budget across nested receivers.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
