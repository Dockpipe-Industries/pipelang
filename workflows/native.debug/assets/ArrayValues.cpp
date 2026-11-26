// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_array_values;

namespace {

HostedValueResult run(const HostedInstance& model, TaskScope& scope, std::int64_t input)
{
    const std::array<HostedArgument, 1> arguments{input};
    const auto task = model.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("native array test timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void requireFailure(const HostedValueResult& result, std::string_view expected)
{
    if (!result.is_failure() || result.error() != expected)
        throw std::runtime_error("wrong array failure for " + std::string(expected));
}

}

int main()
{
    try {
        const auto created = HostedInstance::bind(proof_array_values, {});
        if (!created.is_success())
            throw std::runtime_error(created.error());

        const auto& model = created.value();
        TaskScope scope;
        for (const auto input : {0, 4, 11, 12, 17, 4096}) {
            const auto result = run(model, scope, input);
            if (!result.is_success())
                throw std::runtime_error(result.error());
            const auto& array = result.value();
            if (array.kind() != HostedValue::Kind::List || array.typeName() != "Array<string>" ||
                array.size() != 2 || array.at(0).text() != "new" || array.at(1).text() != "old")
                throw std::runtime_error("hosted array values changed");
        }
        for (const auto input : {-1, 18, 4097})
            requireFailure(run(model, scope, input), "array size outside admitted range");
        for (const auto input : {14, 15, 16})
            requireFailure(run(model, scope, input), "collection index outside admitted range");
        requireFailure(run(model, scope, 13), "collection owned byte limit exceeded");

        std::cout << "Native arrays preserved snapshots, async transfers, bounds, and ownership limits.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
