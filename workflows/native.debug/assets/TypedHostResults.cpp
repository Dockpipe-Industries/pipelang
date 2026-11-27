// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using namespace pipelang::model;

extern "C" const HostedModel proof_typed_host_results;

namespace {

class TestPort final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.TypedHostResults.Port"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        using ServiceResult = Result<HostedServiceValue>;
        const bool asynchronous = signature == "ReadAsync(int,CancellationToken)";
        if (asynchronous) {
            if (arguments.size() != 2 || !std::holds_alternative<CancellationToken>(arguments[1]))
                return ServiceResult::Failure("untyped cancellation argument");
        } else if (signature != "Read(int)" || arguments.size() != 1) {
            return ServiceResult::Failure("unexpected typed host call");
        }
        if (asynchronous == (std::this_thread::get_id() == caller_))
            return ServiceResult::Failure("incorrect hosted execution context");

        const auto scenario = std::get<std::int64_t>(arguments.front());
        const std::string type = "Result<bool,NativeProof.TypedHostResults.Fault>";
        if (scenario == 0 || (scenario == 4 && !asynchronous))
            return ServiceResult::Success(HostedValue::fromResult(type, Outcome::Success, HostedValue::fromBoolean(false)));
        if (scenario == 1 || scenario == 2) {
            const bool cancelled = scenario == 2;
            const auto error = HostedValue::fromRecord("NativeProof.TypedHostResults.Fault", {
                {"code", HostedValue::fromText(cancelled ? "file.cancelled" : "file.missing")},
                {"detail", HostedValue::fromInteger(cancelled ? 21 : 17)}
            });
            return ServiceResult::Success(HostedValue::fromResult(type, cancelled ? Outcome::Cancelled : Outcome::Failure, error));
        }
        if (scenario == 3)
            return ServiceResult::Failure("transport unavailable");
        return ServiceResult::Success(HostedValue::fromInteger(10));
    }

private:
    std::thread::id caller_ = std::this_thread::get_id();
};

HostedResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedArgument, 2> arguments{scenario, CancellationToken{}};
    const auto task = instance.start(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("typed host call did not complete");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    const std::array<HostedConstructorArgument, 1> arguments{std::make_shared<TestPort>()};
    const auto created = HostedInstance::bind(proof_typed_host_results, arguments);
    if (!created.is_success()) {
        std::cerr << created.error() << '\n';
        return 1;
    }
    TaskScope scope;
    for (std::int64_t scenario = 0; scenario < 3; ++scenario) {
        const auto result = run(created.value(), scope, scenario);
        if (!result.is_success() || result.value() != 42 + scenario) {
            std::cerr << result.error() << '\n';
            return 2;
        }
    }
    const auto transport = run(created.value(), scope, 3);
    const auto malformed = run(created.value(), scope, 4);
    if (!transport.is_failure() || transport.error() != "transport unavailable" ||
        !malformed.is_failure() || malformed.error() != "native hosted input does not match its checked nominal type") {
        std::cerr << transport.error() << '\n' << malformed.error() << '\n';
        return 3;
    }
    std::cout << "Typed host results preserved success, failure and cancellation in both call paths.\n";
}
