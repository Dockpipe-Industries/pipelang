// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/di/ScopeExecution.hpp>
#include "ExecutionCounterState.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace pipelang::di;
using namespace pipelang::model;
using execution_test::CounterState;
using execution_test::CounterTag;
using Counter = ObjectRef<CounterTag>;
std::size_t checks = 0;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}

Counter create(const std::shared_ptr<ScopeExecution>& scope, std::int64_t value,
               std::vector<std::int64_t>& trace, bool fails = false, bool exhausts = false) {
    auto result = scope->construct<Counter>({}, [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
        auto reservation = attempt.reserve<Counter>(scope, "fixture.counter",
            [&trace, fails, exhausts](Counter counter, ObjectExecution& cleanup, CancellationToken token) {
                require(!token.is_cancelled(), "cleanup inherited request cancellation");
                trace.push_back(cleanup.transaction.read<CounterState>(counter).value);
                if (exhausts) {
                    for (std::size_t step = 0; step <= EvaluationBudget::maximumSteps; ++step)
                        cleanup.budget.step();
                }
                return fails ? Result<void>::Failure("fixture.cleanup") : Result<void>::Success();
            });
        const auto mark = attempt.allocationMark();
        auto counter = execution.transaction.create<CounterState>(CounterTag{});
        execution.transaction.mutate<CounterState>(counter, [value](CounterState& state) { state.value = value; });
        attempt.adopt(reservation, counter, mark);
        return counter;
    });
    require(result.is_success(), "resource construction failed");
    return result.value();
}
void orderAndProgress() {
    std::vector<std::int64_t> trace;
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto first = create(root, 1, trace);
    auto scope = owner->createScope(root).value();
    auto second = create(scope, 2, trace, true);
    auto third = create(root, 3, trace);
    (void)first; (void)second; (void)third;
    require(root->close().is_failure(), "cleanup failure disappeared");
    require(trace == std::vector<std::int64_t>({3, 2, 1}), "cross-lifetime reverse adoption order changed");
    const auto status = root->cleanupStatus().value();
    require(status.phase == ScopePhase::Closed && status.pending == 0 && status.attempted == 3 &&
            status.succeeded == 2 && status.failed == 1 && status.unknown == 1,
            "attempted/unknown cleanup diagnostics incorrect");
    require(root->close().is_failure() && trace.size() == 3, "closed failure replayed cleanup");
    require(scope->cleanupStatus().value().phase == ScopePhase::Closed, "root did not close descendants");
}
void unwindBeforeDiscard() {
    std::vector<std::int64_t> trace;
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto retained = create(root, 9, trace);
    CancellationSource cancelled;
    const auto result = root->construct<void>(cancelled.token(), [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
        auto reservation = attempt.reserve<Counter>(root, "unpublished", [&](Counter ref, ObjectExecution& cleanup, CancellationToken token) {
            require(!token.is_cancelled(), "request token leaked into unwind");
            trace.push_back(cleanup.transaction.read<CounterState>(ref).value);
            return Result<void>::Failure("secondary.cleanup");
        });
        const auto mark = attempt.allocationMark();
        auto ref = execution.transaction.create<CounterState>(CounterTag{});
        execution.transaction.mutate<CounterState>(ref, [](CounterState& state) { state.value = 42; });
        attempt.adopt(reservation, ref, mark);
        cancelled.cancel();
        throw ExecutionFailure("primary.factory");
    });
    require(result.is_failure() && result.error() == "primary.factory", "cleanup replaced primary construction failure");
    require(trace == std::vector<std::int64_t>({42}), "unpublished graph was discarded before cleanup");
    require(root->invoke<std::int64_t>({}, [&](ObjectExecution& execution) {
        return execution.transaction.read<CounterState>(retained).value;
    }).value() == 9, "failed attempt cleaned a published dependency");
    (void)root->close();
    require(trace == std::vector<std::int64_t>({42, 9}), "published dependency not retained until close");
}
void ownershipChecksAndCancellation() {
    std::vector<std::int64_t> trace;
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto old = create(root, 1, trace);
    const auto stale = root->construct<void>({}, [&](ObjectExecution&, ConstructionAttempt& attempt) {
        auto reserved = attempt.reserve<Counter>(root, "stolen", [&](Counter, ObjectExecution&, CancellationToken) {
            trace.push_back(99);
            return Result<void>::Success();
        });
        attempt.adopt(reserved, old, attempt.allocationMark());
    });
    require(stale.is_failure() && trace.empty(), "old factory result was adopted or spuriously cleaned");
    CancellationSource request;
    const auto cancelled = root->construct<Counter>(request.token(), [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
        auto reserved = attempt.reserve<Counter>(root, "cancelled", [&](Counter, ObjectExecution&, CancellationToken token) {
            require(!token.is_cancelled(), "unwind cleanup token cancelled");
            trace.push_back(2);
            return Result<void>::Success();
        });
        auto mark = attempt.allocationMark();
        auto counter = execution.transaction.create<CounterState>(CounterTag{});
        attempt.adopt(reserved, counter, mark);
        request.cancel();
        return counter;
    });
    require(cancelled.is_cancelled() && trace == std::vector<std::int64_t>({2}), "late factory cancellation published a resource");
    require(root->close().is_success(), "successful cleanup status corrupted");
}
void boundedResumeAndFinalRelease() {
    std::vector<std::int64_t> trace;
    {
        auto owner = ExecutionOwner::Create().value();
        auto root = owner->rootExecution().value();
        auto first = create(root, 1, trace);
        auto second = create(root, 2, trace, false, true);
        (void)first; (void)second;
        require(root->close().is_failure(), "exhausted session reported success");
        auto status = root->cleanupStatus().value();
        require(status.phase == ScopePhase::Closing && status.pending == 1 && status.attempted == 1 &&
                status.failed == 1 && status.unknown == 1, "budget exhaustion lost uncalled record");
        require(root->close().is_failure(), "unknown cleanup outcome disappeared on resume");
        status = root->cleanupStatus().value();
        require(status.phase == ScopePhase::Closed && status.pending == 0 && status.attempted == 2,
                "bounded close did not resume uncalled record");
        require(trace == std::vector<std::int64_t>({2, 1}), "claimed cleanup replayed after exhaustion");
    }
    trace.clear();
    std::weak_ptr<ExecutionOwner> weak;
    {
        auto owner = ExecutionOwner::Create().value();
        weak = owner;
        auto root = owner->rootExecution().value();
        auto scope = owner->createScope(root).value();
        auto resource = create(scope, 7, trace);
        (void)resource;
    }
    require(weak.expired() && trace == std::vector<std::int64_t>({7}), "final owner release leaked resource or owner cycle");
}
void ownerRetainedThroughCleanupCallback() {
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    std::weak_ptr<ExecutionOwner> lifetime = owner;
    int closed = 0;
    const auto created = root->construct<Counter>({}, [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
        const auto reserved = attempt.reserve<Counter>(root, "callback.final_owner_release",
            [&](Counter, ObjectExecution&, CancellationToken) {
                root.reset();
                owner.reset();
                ++closed;
                return Result<void>::Success();
            });
        const auto mark = attempt.allocationMark();
        const auto ref = execution.transaction.create<CounterState>(CounterTag{});
        attempt.adopt(reserved, ref, mark);
        return ref;
    });
    require(created.is_success(), "final callback owner fixture not constructed");
    auto* rawOwner = owner.get();
    require(rawOwner->close().is_success() && closed == 1 && lifetime.expired(),
            "cleanup callback destroyed its executing owner or leaked a lease");
}
}
int main() {
    try {
        orderAndProgress();
        unwindBeforeDiscard();
        ownershipChecksAndCancellation();
        boundedResumeAndFinalRelease();
        ownerRetainedThroughCleanupCallback();
        std::cout << "PASS " << checks << " resource journal assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
