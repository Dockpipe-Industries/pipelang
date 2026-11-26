// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using namespace pipelang::model;

extern "C" const HostedModel proof_persistent_sets;

namespace {

HostedInstance instance(std::size_t extra = 0)
{
    std::vector<HostedValue> initial{HostedValue::fromText("seed")};
    for (std::size_t index = 0; index < extra; ++index)
        initial.push_back(HostedValue::fromText("bulk-" + std::to_string(index)));
    const std::array<HostedConstructorArgument, 1> arguments{
        HostedValue::fromHashSet("HashSet<string>", std::move(initial))
    };
    const auto result = HostedInstance::bind(proof_persistent_sets, arguments);
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return result.value();
}

Task<HostedResult> start(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    return instance.start(scope, arguments);
}

HostedResult finish(TaskScope& scope, const Task<HostedResult>& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("persistent set invocation timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void check(const HostedResult& result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    if (result.value() != 42)
        throw std::runtime_error("unexpected persistent set result");
}

}

int main()
{
    try {
        TaskScope scope;
        const auto model = instance();
        const auto mutation = start(model, scope, 0);
        if (mutation.completed())
            throw std::runtime_error("persistent set fixture did not suspend");
        check(finish(scope, mutation));
        check(finish(scope, start(model, scope, 1)));
        const auto failed = finish(scope, start(model, scope, 2));
        if (!failed.is_failure() || failed.error() != "rollback persistent sets")
            throw std::runtime_error("persistent set rollback lost the source fault");
        check(finish(scope, start(model, scope, 1)));
        check(finish(scope, start(model, scope, 3)));
        check(finish(scope, start(model, scope, 4)));
        check(finish(scope, start(instance(), scope, 5)));
        check(finish(scope, start(instance(1100), scope, 6)));
        std::cout << "Persistent HashSet fields preserved constructor values, aliasing, suspension, and rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
