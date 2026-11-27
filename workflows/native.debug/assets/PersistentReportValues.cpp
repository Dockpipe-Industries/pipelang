// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_persistent_report_values;

namespace {

HostedValueResult run(const HostedInstance& model, TaskScope& scope, std::int64_t input)
{
    const std::array<HostedArgument, 1> arguments{input};
    const auto task = model.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("persistent report test timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void requireFailure(const HostedValueResult& result, std::string_view expected)
{
    if (!result.is_failure() || result.error() != expected)
        throw std::runtime_error("wrong persistent report failure for " + std::string(expected));
}

void requireValue(const HostedValueResult& result, std::int64_t expected)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    if (result.value().integer() != expected)
        throw std::runtime_error("unexpected persistent report result");
}

}

int main()
{
    try {
        const std::array<HostedConstructorArgument, 2> bindings{
            HostedValue::fromList("List<int>", {HostedValue::fromInteger(42)}),
            HostedValue::fromList("List<string>", {})
        };
        const auto created = HostedInstance::bind(proof_persistent_report_values, bindings);
        const auto separate = HostedInstance::bind(proof_persistent_report_values, bindings);
        if (!created.is_success())
            throw std::runtime_error(created.error());
        if (!separate.is_success())
            throw std::runtime_error(separate.error());

        TaskScope scope;
        const auto& first = created.value();
        requireValue(run(first, scope, 0), 1);
        requireValue(run(first, scope, 2), 42);
        requireFailure(run(first, scope, 1), "rollback persistent reports");
        requireValue(run(first, scope, 2), 42);
        requireValue(run(separate.value(), scope, 3), 42);

        const auto message = HostedValue::fromText(std::string(200, 'a'));
        std::vector<HostedValue> messages(4000, message);
        auto bulkBindings = bindings;
        bulkBindings[1] = HostedValue::fromList("List<string>", std::move(messages));
        const auto bulk = HostedInstance::bind(proof_persistent_report_values, bulkBindings);
        if (!bulk.is_success())
            throw std::runtime_error(bulk.error());
        requireValue(run(bulk.value(), scope, 4), 16000);
        std::cout << "Native report values survived suspension, write rollback, and separate bindings.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
