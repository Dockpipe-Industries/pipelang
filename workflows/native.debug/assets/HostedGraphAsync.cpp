// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifndef PIPELANG_GRAPH_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_graph_async;
#endif

template<class Start, class Poll>
pipelang::model::Result<std::int64_t> run(const Start& start, const Poll& poll, std::int64_t scenario)
{
    pipelang::model::CancellationSource cancellation;
    const auto task = start(scenario, cancellation.token());
    if (task.completed())
        throw std::runtime_error(task.result().is_failure() ? task.result().error() : "hosted graph did not suspend");
    if (scenario == 1)
        cancellation.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted graph did not complete");
        poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

int main()
{
    try {
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 33554432;
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
        reference::source::NativeProof::HostedGraphAsync::Session instance(options);
        const auto start = [&](std::int64_t scenario, pipelang::model::CancellationToken token) {
            return instance.Run(scenario, token);
        };
        const auto poll = [&] { instance.poll(); };
#else
        const auto instance = HostedInstance::bind(proof_hosted_graph_async, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope(options);
        const auto start = [&](std::int64_t scenario, pipelang::model::CancellationToken token) {
            const std::array<HostedArgument, 2> arguments{scenario, token};
            return instance.value().start(scope, arguments);
        };
        const auto poll = [&] { scope.poll(); };
#endif
        const auto initial = run(start, poll, 0);
        if (!initial.is_success() || initial.value() != 42)
            throw std::runtime_error(initial.is_failure() ? initial.error() : "wrong initial graph state");

        if (!run(start, poll, 1).is_cancelled())
            throw std::runtime_error("hosted graph did not observe cancellation");

        const auto retained = run(start, poll, 2);
        if (!retained.is_success() || retained.value() != 46)
            throw std::runtime_error(retained.is_failure() ? retained.error() : "wrong retained graph state");

        std::cout << "Hosted graph synchronization survived workers, suspension, and cancellation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
