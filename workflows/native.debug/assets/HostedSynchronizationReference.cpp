// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::HostedSynchronization::Session;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const auto task = session.Run(scenario, cancellation.token());
    if (task.completed())
        throw std::runtime_error(task.result().is_failure() ? task.result().error() : "reference fixture did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference fixture did not complete");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    try {
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        Session session(options);
        if (run(session, 0).value() != 41 || !run(session, 1).is_cancelled())
            throw std::runtime_error("reference hosted result or cancellation changed");

        const auto failure = run(session, 2);
        if (!failure.is_failure() || failure.error() != "expected hosted failure")
            throw std::runtime_error("reference hosted source failure changed");
        if (run(session, 0).value() != 44)
            throw std::runtime_error("reference hosted shared state was lost after failure or cancellation");

        Session separate(options);
        if (run(separate, 0).value() != 41)
            throw std::runtime_error("reference hosted instances shared state");

        std::cout << "C++ hosted synchronization and concurrent collections preserved their lifetime.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
