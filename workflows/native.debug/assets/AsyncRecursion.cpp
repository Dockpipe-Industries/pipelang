// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_async_recursion;

namespace {

HostedResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario, bool cancel = false)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    const auto task = instance.start(scope, arguments);
    if (scenario != 30 && task.completed())
        throw std::runtime_error("recursive call failed to suspend");
    if (cancel)
        task.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("recursive call timed out");
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
        const auto instance = HostedInstance::bind(proof_async_recursion, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());

        const auto& session = instance.value();
        TaskScope scope;
        if (run(session, scope, 4).value() != 14 || run(session, scope, -3).value() != 17 ||
            run(session, scope, 29).value() != 39)
            throw std::runtime_error("recursive call lost its result across suspension");
        const auto limited = run(session, scope, 30);
        if (!limited.is_failure() || limited.error() != "source call depth limit exceeded")
            throw std::runtime_error("recursive call did not enforce the source depth limit");
        if (!run(session, scope, 3, true).is_cancelled() || run(session, scope, 0).value() != 10)
            throw std::runtime_error("recursive cancellation left the hosted receiver unusable");

        std::cout << "Native async recursion preserved results, suspension, cancellation and the source depth limit.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
