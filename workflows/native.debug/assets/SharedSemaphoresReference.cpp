// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::SharedSemaphores::Session;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const auto task = session.Run(scenario, cancellation.token());
    if (task.completed())
        throw std::runtime_error("reference semaphore did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference semaphore failed to complete");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    pipelang::model::TaskOptions options;
    options.evaluationStepsPerSegment = 33554432;
    Session session(options);
    if (run(session, 0).value() != 42 || !run(session, 1).is_cancelled() || run(session, 0).value() != 42)
        return 1;

    std::cout << "The C++ reference agreed on semaphore identity, suspension and cancellation.\n";
}
