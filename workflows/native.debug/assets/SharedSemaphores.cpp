// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_shared_semaphores;

namespace {

HostedResult run(const HostedInstance& instance, pipelang::model::TaskScope& scope, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const std::array<HostedArgument, 2> arguments{scenario, cancellation.token()};
    const auto task = instance.start(scope, arguments);
    if (task.completed())
        throw std::runtime_error("forwarded semaphore did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("forwarded semaphore failed to complete");
        scope.poll();
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
        const auto instance = HostedInstance::bind(proof_shared_semaphores, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope(options);
        const auto& session = instance.value();
        if (run(session, scope, 0).value() != 42 || !run(session, scope, 1).is_cancelled() ||
            run(session, scope, 0).value() != 42)
            throw std::runtime_error("forwarded semaphore lost its result or cancellation");

        std::cout << "Native semaphores preserved aliases, child waits, default fields and construction rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
