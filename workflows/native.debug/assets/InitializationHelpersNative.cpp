// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_initialization_helpers;

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

auto bind(std::size_t steps)
{
    const std::array<HostedConstructorArgument, 1> input{std::int64_t{5000}};
    return HostedInstance::bind(proof_initialization_helpers, input, steps);
}

int main()
{
    try {
        const auto limited = bind(65536);
        require(limited.is_failure() && limited.error() == "source evaluation budget exceeded",
                limited.is_success() ? "each initialization helper received a fresh budget" : limited.error());
        const auto model = bind(131072);
        require(model.is_success(), model.is_success() ? "" : model.error());

        pipelang::model::TaskScope scope;
        const std::array<HostedArgument, 1> smallInput{std::int64_t{4}};
        const auto query = model.value().start(scope, smallInput).result();
        require(query.is_success() && query.value() == 4, "invocation query had the wrong budget or value");

        const std::array<HostedCallArgument, 1> largeInput{std::int64_t{10000}};
        const auto exhausted = model.value().startMethod(scope, "Consume", largeInput).result();
        require(exhausted.is_failure() && exhausted.error() == "source evaluation budget exceeded",
                exhausted.is_success() ? "invocation reused the higher initialization budget" : exhausted.error());
        std::cout << "Constructor helper calls share the initialization budget. Each invocation gets its own budget.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
