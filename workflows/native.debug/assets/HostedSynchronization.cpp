// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_hosted_synchronization;

namespace {

HostedResult run(const HostedInstance& session, pipelang::model::TaskScope& scope, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const std::array<HostedArgument, 2> arguments{scenario, cancellation.token()};
    const auto task = session.start(scope, arguments);
    if (task.completed())
        throw std::runtime_error(task.result().is_failure() ? task.result().error() : "hosted fixture did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted fixture did not complete");
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
        pipelang::model::TaskScope scope(options);

        const auto instance = HostedInstance::bind(proof_hosted_synchronization, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        const auto& session = instance.value();
        if (run(session, scope, 0).value() != 41 || !run(session, scope, 1).is_cancelled())
            throw std::runtime_error("native hosted result or cancellation changed");

        const auto failure = run(session, scope, 2);
        if (!failure.is_failure() || failure.error() != "expected hosted failure")
            throw std::runtime_error("native hosted source failure changed");
        if (run(session, scope, 0).value() != 44)
            throw std::runtime_error("native hosted shared state was lost after failure or cancellation");

        const auto separate = HostedInstance::bind(proof_hosted_synchronization, {});
        if (!separate.is_success() || run(separate.value(), scope, 0).value() != 41)
            throw std::runtime_error("native hosted instances shared state");

        std::cout << "Native hosted synchronization and concurrent collections preserved their lifetime.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
