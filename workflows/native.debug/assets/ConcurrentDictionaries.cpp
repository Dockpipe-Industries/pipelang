// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>
#include <pipelang/model/concurrent_collections.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_concurrent_dictionaries;

namespace {

struct ProjectionFailure {};

void verifyFailedProjections()
{
    const auto reject = [](const auto&) { throw ProjectionFailure{}; };
    const auto check = [&](const auto& operation) {
        try {
            operation(reject);
        } catch (const ProjectionFailure&) {
            return;
        }
        throw std::runtime_error("projected operation did not propagate the failure");
    };

    const auto queue = pipelang::model::ConcurrentQueue<std::int64_t>::Empty();
    queue.enqueue(42);
    check([&](const auto& projection) { queue.tryDequeueProjected(projection); });
    if (queue.count() != 1 || queue.tryDequeue().value() != 42)
        throw std::runtime_error("failed projection removed the queue item");

    const auto stack = pipelang::model::ConcurrentStack<std::int64_t>::Empty();
    stack.push(42);
    check([&](const auto& projection) { stack.tryPopProjected(projection); });
    if (stack.count() != 1 || stack.tryPop().value() != 42)
        throw std::runtime_error("failed projection removed the stack item");

    const auto dictionary = pipelang::model::ConcurrentDictionary<std::int64_t, std::int64_t>::Empty();
    dictionary.set(1, 42);
    check([&](const auto& projection) { dictionary.tryRemoveProjected(1, projection); });
    check([&](const auto& projection) { dictionary.getOrAddProjected(2, 42, projection); });
    if (dictionary.count() != 1 || dictionary.at(1) != 42)
        throw std::runtime_error("failed projection changed the dictionary");
}

HostedResult run(const HostedInstance& instance, pipelang::model::TaskScope& scope, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const std::array<HostedArgument, 2> arguments{scenario, cancellation.token()};
    const auto task = instance.start(scope, arguments);
    if (task.completed())
        throw std::runtime_error("concurrent collection test did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("concurrent collection test failed to complete");
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
        verifyFailedProjections();
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
        const auto instance = HostedInstance::bind(proof_concurrent_dictionaries, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope(options);
        const auto& session = instance.value();
        if (run(session, scope, 0).value() != 42 || !run(session, scope, 1).is_cancelled() ||
            run(session, scope, 0).value() != 42)
            throw std::runtime_error("concurrent collection test lost its result or cancellation");

        std::cout << "Native concurrent collections preserved shared identity and owned snapshots.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
