// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationSource;
using pipelang::model::CancellationToken;
using pipelang::model::TaskScope;
using Response = pipelang::model::Result<HostedServiceValue>;

extern "C" const HostedModel proof_ordinary;
extern "C" const HostedModel proof_probe;
extern "C" const HostedModel proof_async;

namespace {

void require(bool condition, std::string_view error)
{
    if (!condition)
        throw std::runtime_error(std::string(error));
}

HostedValue checked(const HostedValueResult& result)
{
    require(result.is_success(), result.is_success() ? "" : result.error());
    return result.value();
}

HostedInstance bind(const HostedModel& model, std::initializer_list<HostedConstructorArgument> arguments = {})
{
    auto instance = HostedInstance::bind(model, arguments);
    require(instance.is_success(), instance.is_success() ? "" : instance.error());
    return std::move(instance).value();
}

void ordinary()
{
    auto suite = bind(proof_ordinary);
    TaskScope scope;
    const std::array<HostedArgument, 1> active{CancellationToken{}};
    const auto passed = checked(suite.startValue(scope, active).result());
    require(passed.size() == 5, "source ordinary fixture registration count changed");
    for (std::size_t index = 0; index < passed.size(); ++index) {
        const auto test = passed.at(index);
        require(test.field("outcome").enumerator() == "Passed", test.field("id").text());
    }

    const std::array<HostedCallArgument, 1> ownedActive{CancellationToken{}};
    require(checked(suite.startMethod(scope, "DuplicateRegistrationRefused", ownedActive).result()).boolean(),
            "duplicate registration was not refused");
    const auto failed = checked(suite.startMethod(scope, "DeliberateFailure", ownedActive).result());
    require(failed.field("outcome").enumerator() == "Failed" && failed.field("primaryOutcome").enumerator() == "Failed" &&
            failed.field("primaryPhase").enumerator() == "Body" &&
            failed.field("primaryFailure").value().field("message").text() == "body" &&
            failed.field("cleanupFailure").value().field("message").text() == "cleanup" &&
            failed.field("cleanupRan").boolean(), "structured primary and cleanup failure changed");

    CancellationSource cancellation;
    cancellation.cancel();
    const std::array<HostedArgument, 1> cancelledInput{cancellation.token()};
    const auto cancelled = checked(suite.startValue(scope, cancelledInput).result());
    require(cancelled.size() == 5, "cancellation removed source case identities");
    for (std::size_t index = 0; index < cancelled.size(); ++index) {
        const auto result = cancelled.at(index);
        require(result.field("outcome").enumerator() == "Cancelled" && !result.field("bodyRan").boolean() &&
                !result.field("cleanupRan").boolean(), "pre-cancelled case entered its lifecycle");
    }
    std::cout << "PipeLang authored tests: 5 passed; duplicate, dual failure and cancellation contracts passed\n";
}

class ProbeControl final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "PipeLang.Testing.SelfTests.ProbeCancellationControl";
    }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> args) override
    {
        require(signature == "RequestCancellation(int)" && args.size() == 1, "unexpected cancellation control signature");
        const auto index = std::get<std::int64_t>(args.front());
        if (index < 0 || static_cast<std::uint64_t>(index) >= sources_.size())
            return Response::Failure("probe.cancellation_scope");
        sources_[index].cancel();
        return Response::Success(HostedValue{});
    }

    CancellationToken token(std::size_t index) const { return sources_.at(index).token(); }

private:
    std::array<CancellationSource, 4> sources_;
};

nlohmann::ordered_json reportedFailure(const HostedValue& optional)
{
    if (!optional.hasValue())
        return nullptr;
    const auto failure = optional.value();
    return {{"code", failure.field("code").text()}, {"message", failure.field("message").text()}};
}

void probe()
{
    const auto control = std::make_shared<ProbeControl>();
    const auto suite = bind(proof_probe, {control});
    TaskScope scope;
    const std::array<HostedArgument, 5> tokens{CancellationToken{}, control->token(0), control->token(1),
        control->token(2), control->token(3)};
    const auto report = checked(suite.startValue(scope, tokens).result());
    const auto results = report.field("results");
    require(results.size() == 9, "probe suite registration changed");
    auto rows = nlohmann::ordered_json::array();
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto result = results.at(index);
        rows.push_back({
            {"id", result.field("id").text()},
            {"name", result.field("name").text()},
            {"outcome", result.field("outcome").ordinal()},
            {"primaryOutcome", result.field("primaryOutcome").ordinal()},
            {"primaryPhase", result.field("primaryPhase").ordinal()},
            {"primaryFailure", reportedFailure(result.field("primaryFailure"))},
            {"cleanupOutcome", result.field("cleanupOutcome").ordinal()},
            {"cleanupFailure", reportedFailure(result.field("cleanupFailure"))},
            {"bodyRan", result.field("bodyRan").boolean()},
            {"cleanupRan", result.field("cleanupRan").boolean()}
        });
    }
    const auto allPassed = report.field("allPassed").boolean();
    const nlohmann::ordered_json result{
        {"schema", "pipelang.testing-probe-contracts/v1"},
        {"allPassed", allPassed},
        {"results", std::move(rows)}
    };
    std::cout << result.dump() << '\n';
    require(allPassed, "authored cancellation probe failed");
}

struct FixtureState {
    CancellationSource bodyCancellation;
    std::atomic<bool> rootReleased{false};
    const std::thread::id owner = std::this_thread::get_id();
    std::int64_t milliseconds = 100;
    std::atomic<std::int64_t> readsBeforeFailure{-1};
};

class Clock final : public HostedService {
public:
    explicit Clock(FixtureState& state) : state_(state) {}
    std::string_view interfaceName() const noexcept override { return "PipeLang.Testing.Async.TestClock"; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> args) override
    {
        require(signature == "NowMilliseconds()" && args.empty(), "unexpected test clock operation");
        const auto remaining = state_.readsBeforeFailure.load();
        if (remaining == 0) {
            state_.readsBeforeFailure = -1;
            return Response::Failure("fixture clock unavailable");
        }
        if (remaining > 0)
            state_.readsBeforeFailure = remaining - 1;
        return Response::Success(state_.milliseconds);
    }

private:
    FixtureState& state_;
};

class AsyncFixture final : public HostedService {
public:
    explicit AsyncFixture(FixtureState& state) : state_(state) {}

    std::string_view interfaceName() const noexcept override
    {
        return "PipeLang.Testing.AsyncSelfTests.AsyncFixture";
    }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> args) override
    {
        if (signature == "AdvanceMilliseconds(int)") {
            const auto increment = std::get<std::int64_t>(args[0]);
            if (increment < 0 || state_.milliseconds > std::numeric_limits<std::int64_t>::max() - increment)
                return Response::Failure("invalid fixture clock advance");
            state_.milliseconds += increment;
        } else if (signature == "FailClockAfter(int)") {
            const auto reads = std::get<std::int64_t>(args[0]);
            if (reads < 0 || reads > 128)
                return Response::Failure("invalid fixture clock failure schedule");
            state_.readsBeforeFailure = reads;
        } else if (signature == "RequestBodyCancellation()") {
            state_.bodyCancellation.cancel();
        } else if (signature == "Observe(string,int,CancellationToken)") {
            return observe(std::get<std::string>(args[0]), std::get<std::int64_t>(args[1]),
                           std::get<CancellationToken>(args[2]));
        } else {
            return Response::Failure("unexpected async test fixture signature");
        }
        return Response::Success(HostedValue{});
    }

private:
    Response observe(std::string_view phase, std::int64_t remaining, const CancellationToken& token)
    {
        if (phase.empty() || remaining <= 0)
            return Response::Failure("observation requires a phase and remaining budget");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<std::int64_t>(remaining, 3000));
        while (!state_.rootReleased.load()) {
            if (token.is_cancelled())
                return Response::Cancelled("fixture observation cancelled");
            if (std::chrono::steady_clock::now() >= deadline)
                return Response::Failure("fixture observation gate expired");
            std::this_thread::yield();
        }
        if (token.is_cancelled())
            return Response::Cancelled("fixture observation cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Response::Success(std::this_thread::get_id() != state_.owner);
    }

    FixtureState& state_;
};

void asynchronous()
{
    FixtureState state;
    const auto suite = bind(proof_async, {std::make_shared<Clock>(state), std::make_shared<AsyncFixture>(state)});
    TaskScope scope;
    CancellationSource cancellation;
    cancellation.cancel();
    const std::array<HostedArgument, 2> args{state.bodyCancellation.token(), cancellation.token()};
    const auto task = suite.startValue(scope, args);
    state.rootReleased = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!task.completed()) {
        scope.poll();
        require(std::chrono::steady_clock::now() < deadline, "async source suite exceeded the native fixture watchdog");
        std::this_thread::yield();
    }
    const auto results = checked(task.result());
    require(results.size() == 25, "async source fixture registration count changed");
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto test = results.at(index).field("test");
        require(test.field("outcome").enumerator() == "Passed", test.field("id").text());
    }
    std::cout << "PipeLang source async test suite: " << results.size() << " passed\n";
}

}

int main()
{
    try {
        ordinary();
        probe();
        asynchronous();
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
