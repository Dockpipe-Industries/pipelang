// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_constructor_rollback;

int main()
{
    try {
        const auto session = HostedInstance::bind(proof_constructor_rollback, {});
        if (!session.is_success())
            throw std::runtime_error(session.error());

        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        pipelang::model::TaskScope scope(options);
        constexpr std::array<std::int64_t, 5> expected{42, 77, 77, -1, 77};
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const std::array<HostedArgument, 1> inputs{static_cast<std::int64_t>(index)};
            const auto result = session.value().start(scope, inputs).result();
            if (index == 3) {
                if (!result.is_failure() || result.error() != "outer invocation failed")
                    throw std::runtime_error("outer failure changed");
                continue;
            }
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value() != expected[index])
                throw std::runtime_error("constructor savepoint changed receiver state");
        }

        std::cout << "Native constructor savepoints preserved receiver state and reclaimed failed allocations.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
