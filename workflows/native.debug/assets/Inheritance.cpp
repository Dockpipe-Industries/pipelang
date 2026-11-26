// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_inheritance;

namespace {

HostedResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario, bool cancel = false)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    const auto task = instance.start(scope, arguments);
    if (task.completed())
        throw std::runtime_error("virtual call did not suspend");
    if (cancel)
        task.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("virtual call timed out");
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
        const auto instance = HostedInstance::bind(proof_inheritance, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        const auto& session = instance.value();
        TaskScope scope;
        constexpr std::array<std::int64_t, 5> expected{34, 89, 84, 37, 42};
        for (std::size_t scenario = 0; scenario < expected.size(); ++scenario) {
            const auto result = run(session, scope, static_cast<std::int64_t>(scenario));
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value() != expected[scenario])
                throw std::runtime_error("inherited receiver layout or virtual dispatch changed");
        }
        if (!run(session, scope, 1, true).is_cancelled() || run(session, scope, 1).value() != 89)
            throw std::runtime_error("cancellation left virtual dispatch unusable");

        std::cout << "Native inheritance preserved base state, virtual dispatch, optional receivers and cancellation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
