// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::ResultInterface::Session;
using namespace pipelang::model;

int main()
{
    try {
        TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        Session session(options);

        for (const auto scenario : {0, 1, 0}) {
            CancellationSource cancellation;
            const auto task = session.Run(scenario, cancellation.token());
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
                session.poll();
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
