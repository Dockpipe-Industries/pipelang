// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::model;
using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_hosted_receivers;

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

HostedValue makeInputs(std::int64_t seed, std::string label)
{
    constexpr std::array<std::uint8_t, 7> bytes{'i', 'n', 'i', 't', 'i', 'a', 'l'};
    return HostedValue::fromRecord("NativeProof.HostedReceivers.Inputs", {
        {"note", HostedValue::fromText("")},
        {"route", HostedValue::fromRecord("NativeProof.HostedReceivers.Route", {
            {"enabled", HostedValue::fromBoolean(true)},
            {"payload", HostedValue::fromBytes(bytes)},
            {"label", HostedValue::fromText(std::move(label))}
        })},
        {"paths", HostedValue::fromRecord("NativeProof.HostedReceivers.Paths", {
            {"source", HostedValue::fromText("src")},
            {"work", HostedValue::fromText("work")},
            {"output", HostedValue::fromText("out")},
            {"log", HostedValue::fromText("logs")},
            {"cache", HostedValue::fromText("cache")},
            {"input", HostedValue::fromText("inputs")},
            {"state", HostedValue::fromText("state")},
            {"manifest", HostedValue::fromText("manifest.json")}
        })},
        {"seed", HostedValue::fromInteger(seed)}
    });
}

Result<HostedInstance> bind(HostedValue value)
{
    const std::array<HostedConstructorArgument, 1> arguments{std::move(value)};
    return HostedInstance::bind(proof_hosted_receivers, arguments);
}

HostedInstance create(std::int64_t seed)
{
    const auto created = bind(makeInputs(seed, "étape"));
    if (!created.is_success())
        throw std::runtime_error(created.error());
    return created.value();
}

Task<HostedResult> run(TaskScope& scope, const HostedInstance& instance, std::int64_t scenario)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    return instance.start(scope, arguments);
}

HostedResult finish(TaskScope& scope, const Task<HostedResult>& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        require(std::chrono::steady_clock::now() < deadline, "nested receiver timed out");
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
        TaskScope scope;
        const auto invalid = bind(makeInputs(-1, "étape"));
        require(invalid.is_failure() && invalid.error() == "native hosted construction read an uninitialized receiver field",
                "hosted construction published an incomplete object graph");
        require(bind(HostedValue::fromRecord("Other.Inputs", {})).is_failure(), "host accepted a different constructor record");
        require(bind(makeInputs(40, "\xff")).is_failure(), "host accepted malformed UTF-8 in a nested constructor record");
        const auto first = create(40);
        const auto second = create(80);
        const auto suspended = run(scope, first, 0);
        require(!suspended.completed(), "nested receiver failed to suspend");
        require(finish(scope, suspended).value() == 42, "nested receiver lost its constructed state");
        require(finish(scope, run(scope, first, 2)).value() == 44, "two ports failed to preserve shared client identity");
        const auto fault = finish(scope, run(scope, first, 1));
        require(fault.is_failure() && fault.error() == "nested receiver fault", "nested receiver lost its execution fault");
        require(finish(scope, run(scope, first, 2)).value() == 47, "nested receiver rolled back the wrong segment");
        const auto synchronousFault = finish(scope, run(scope, first, 3));
        require(synchronousFault.is_failure() && synchronousFault.error() == "rollback synchronous writes",
                "a synchronous fault lost its error");
        require(finish(scope, run(scope, first, 5)).value() == 0,
                "a synchronous fault left writes in one of two receivers");
        require(finish(scope, run(scope, first, 4)).value() == 52,
                "catch did not preserve writes in the active segment or rollback lost a shared receiver");
        require(finish(scope, run(scope, first, 6)).value() == 53,
                "void return skipped its finally body");
        require(finish(scope, run(scope, second, 0)).value() == 82, "hosted instances shared their object storage");
        require(finish(scope, run(scope, first, 7)).value() == 7, "writing receiver text failed");
        require(finish(scope, run(scope, first, 8)).value() == 8, "receiver text expired when its invocation ended");
        const auto textFault = finish(scope, run(scope, first, 9));
        require(textFault.is_failure() && textFault.error() == "rollback receiver text",
                "a fault while assigning receiver text lost its error");
        require(finish(scope, run(scope, first, 8)).value() == 8, "receiver text did not roll back across receivers");
        require(finish(scope, run(scope, first, 10)).value() == 10, "borrowed receiver text expired on replacement");
        require(finish(scope, run(scope, first, 11)).value() == 11, "receiver text did not keep its committed replacement");
        require(finish(scope, run(scope, second, 12)).value() == 12, "receiver text leaked across hosted instances");
        require(finish(scope, run(scope, first, 13)).value() == 32640,
                "fieldless local receivers accumulated across synchronous calls");
        const auto record = run(scope, first, 14);
        require(!record.completed(), "nested record write did not suspend");
        require(finish(scope, record).value() == 14, "nested record borrow expired after replacement");
        require(finish(scope, run(scope, first, 15)).value() == 15, "nested record ownership did not persist");
        const auto recordFault = finish(scope, run(scope, first, 16));
        require(recordFault.is_failure() && recordFault.error() == "rollback nested record fields",
                "nested record assignment lost its execution fault");
        require(finish(scope, run(scope, first, 15)).value() == 15, "nested record writes did not roll back");
        const auto replacement = run(scope, first, 17);
        require(!replacement.completed(), "whole-record replacement did not suspend");
        require(finish(scope, replacement).value() == 17, "whole-record replacement invalidated a snapshot");
        require(finish(scope, run(scope, first, 18)).value() == 18, "whole-record replacement did not persist");
        const auto replacementFault = finish(scope, run(scope, first, 19));
        require(replacementFault.is_failure() && replacementFault.error() == "rollback whole receiver records",
                "whole-record replacement lost its execution fault");
        require(finish(scope, run(scope, first, 18)).value() == 18, "whole-record replacement did not roll back");
        require(finish(scope, run(scope, first, 20)).value() == 42,
                "scoped or constructor-retained receiver lost its storage");
        const auto scoped = run(scope, first, 21);
        require(!scoped.completed(), "scoped receiver did not suspend");
        require(finish(scope, scoped).value() == 42, "a scoped receiver expired across suspension");
        const auto returned = run(scope, first, 22);
        require(!returned.completed(), "the returned receiver did not cross suspension");
        require(finish(scope, returned).value() == 42, "the returned receiver expired across suspension");
        const auto failed = finish(scope, run(scope, first, 23));
        require(failed.is_failure() && failed.error() == "negative score", "a failing receiver result changed");
        const auto temporary = run(scope, create(100), 2);
        require(!temporary.completed(), "temporary owned graph failed to suspend");
        require(finish(scope, temporary).value() == 102, "suspended task lost its hosted object graph");
        std::cout << "Hosted PipeLang receivers preserved synchronous effects, cleanup and segment rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
