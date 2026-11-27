// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace pipelang::native_app_v19;
using pipelang::model::Outcome;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_hosted_synchronous;

namespace {

HostedValueResult run(const HostedInstance& model, TaskScope& scope, std::int64_t increment)
{
    const std::array<HostedArgument, 1> arguments{increment};
    const auto task = model.startValue(scope, arguments);
    if (!task.completed())
        throw std::runtime_error("synchronous hosted entry suspended");
    return task.result();
}

void requireOutcome(const HostedValueResult& result, Outcome outcome, std::string_view error)
{
    if (result.outcome() != outcome || result.error() != error)
        throw std::runtime_error("synchronous hosted entry returned the wrong outcome");
}

}

int main()
{
    try {
        const auto created = HostedInstance::bind(proof_hosted_synchronous, {});
        if (!created.is_success())
            throw std::runtime_error(created.error());

        TaskScope scope;
        const auto& model = created.value();
        const auto first = run(model, scope, 2);
        if (!first.is_success() || first.value().integer() != 2)
            throw std::runtime_error("synchronous hosted entry did not persist the initial state");
        requireOutcome(run(model, scope, 0), Outcome::Failure, "increment required");
        requireOutcome(run(model, scope, -2), Outcome::Cancelled, "increment cancelled");
        requireOutcome(run(model, scope, -1), Outcome::Failure, "increment rolled back");
        const auto last = run(model, scope, 3);
        if (!last.is_success() || last.value().integer() != 5)
            throw std::runtime_error("synchronous hosted entry did not restore the previous state");

        std::cout << "The native synchronous hosted entry completed inline and preserved its result outcomes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
