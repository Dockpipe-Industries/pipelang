// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using namespace pipelang::model;

extern "C" const HostedModel proof_result_interface;

int main()
{
    try {
        TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        const auto model = HostedInstance::bind(proof_result_interface, {});
        if (!model.is_success())
            throw std::runtime_error(model.error());
        TaskScope scope(options);

        for (const auto scenario : {0, 1, 0}) {
            CancellationSource cancellation;
            const std::array<HostedArgument, 2> arguments{std::int64_t{scenario}, cancellation.token()};
            const auto task = model.value().start(scope, arguments);
            if (task.completed()) {
                const auto early = task.result();
                throw std::runtime_error(early.is_failure() ? early.error() : "result interface did not suspend");
            }
            if (scenario == 1)
                cancellation.cancel();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!task.completed()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("result interface did not complete");
                scope.poll();
                if (!task.completed())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const auto result = task.result();
            if (scenario == 1) {
                if (!result.is_cancelled())
                    throw std::runtime_error("result interface lost cancellation");
                std::cout << "cancelled\n";
            } else {
                if (!result.is_success())
                    throw std::runtime_error(result.error());
                if (result.value() != 42)
                    throw std::runtime_error("result interface returned the wrong result");
                std::cout << result.value() << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
