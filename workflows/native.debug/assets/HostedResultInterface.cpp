// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::Outcome;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_hosted_result_interface;

namespace {

HostedValueResult run(const HostedInstance& model, TaskScope& scope, std::int64_t scenario, HostedValue incoming = {})
{
    const std::array<HostedCallArgument, 2> arguments{
        scenario, HostedValue::fromOptional("Optional<IResult<string>>", std::move(incoming))
    };
    const auto task = model.startWithValues(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted result interface test timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

HostedValue checkedValue(const HostedValueResult& result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    auto value = result.value();
    if (value.kind() != HostedValue::Kind::ResultInterface || value.typeName() != "IResult<string>")
        throw std::runtime_error("hosted result lost the erased type");
    return value;
}

void verifyError(const HostedValue& value)
{
    if (value.outcome() != Outcome::Failure || value.error().text() != "retained error")
        throw std::runtime_error("hosted result lost the erased error");
}

}

int main()
{
    try {
        const std::array<HostedConstructorArgument, 1> bindings{HostedValue::fromBoolean(true)};
        const std::array<HostedConstructorArgument, 1> missing{HostedValue::fromBoolean(false)};
        auto origin = HostedInstance::bind(proof_hosted_result_interface, bindings);
        auto other = HostedInstance::bind(proof_hosted_result_interface, bindings);
        if (!origin.is_success() || !other.is_success())
            throw std::runtime_error(origin.error());
        const auto invalid = HostedInstance::bind(proof_hosted_result_interface, missing);
        if (!invalid.is_failure() || invalid.error() != "result interface is empty")
            throw std::runtime_error("hosted construction admitted an uninitialized result interface");

        TaskScope scope;
        verifyError(checkedValue(run(origin.value(), scope, 5)));
        const auto empty = run(origin.value(), scope, 0);
        if (!empty.is_failure() || empty.error() != "Optional.Value requires Some")
            throw std::runtime_error("an absent result interface admitted observation");

        const auto original = checkedValue(run(origin.value(), scope, 1));
        verifyError(original);
        verifyError(checkedValue(run(origin.value(), scope, 2, original)));
        const auto transferred = checkedValue(run(other.value(), scope, 3, original));
        verifyError(transferred);
        verifyError(checkedValue(run(origin.value(), scope, 2, transferred)));
        const auto failed = run(other.value(), scope, 4);
        if (!failed.is_failure() || failed.error() != "discard this result interface")
            throw std::runtime_error("expected hosted result rollback failure");
        verifyError(checkedValue(run(other.value(), scope, 2, original)));
        std::cout << "Hosted result identity, error storage, and rollback passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
