// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_blocking_semaphores;

int main()
{
    try {
        const auto instance = HostedInstance::bind(proof_blocking_semaphores, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());

        pipelang::model::TaskScope scope;
        for (const bool cancelled : {false, true, false}) {
            pipelang::model::CancellationSource cancellation;
            if (cancelled)
                cancellation.cancel();

            const std::array<HostedArgument, 1> arguments{cancellation.token()};
            const auto task = instance.value().start(scope, arguments);
            if (!task.completed())
                throw std::runtime_error("blocking semaphore suspended its method");

            const auto result = task.result();
            if (cancelled) {
                if (!result.is_cancelled())
                    throw std::runtime_error("blocking semaphore lost cancellation");
                std::cout << "cancelled\n";
            } else {
                if (!result.is_success())
                    throw std::runtime_error(result.error());
                if (result.value() != 42)
                    throw std::runtime_error("blocking semaphore returned the wrong result");
                std::cout << result.value() << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
