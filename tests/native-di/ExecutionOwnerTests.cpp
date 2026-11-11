// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/di/ExecutionOwner.hpp>
#include "ExecutionCounterState.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace pipelang::di;
using namespace pipelang::model;
using execution_test::CounterState;
using execution_test::CounterTag;
using Counter = ObjectRef<CounterTag>;
std::size_t checks = 0;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}

void sharedGraphAndTransactions()
{
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto child = owner->createScope(root).value();
    const auto created = root->invoke<Counter>({}, [](ObjectExecution& execution) {
        auto counter = execution.transaction.create<CounterState>(CounterTag{});
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 7; });
        return counter;
    });
    require(created.is_success(), "root object construction failed");
    const auto counter = created.value();
    const auto read = [&counter](ObjectExecution& execution) {
        return execution.transaction.read<CounterState>(counter).value;
    };
    require(child->invoke<std::int64_t>({}, read).value() == 7, "child did not share root object domain");
    const auto failed = child->invoke<void>({}, [&](ObjectExecution& execution) {
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 42; });
        throw ExecutionFailure("fixture.failure");
    });
    require(failed.is_failure() && failed.error() == "fixture.failure", "source exception lost failure");
    require(root->invoke<std::int64_t>({}, read).value() == 7, "exception graph mutation committed");
    const auto interrupted = child->invoke<void>({}, [&](ObjectExecution& execution) {
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 43; });
        throw OperationCancelled("fixture.cancelled");
    });
    require(interrupted.is_cancelled() && root->invoke<std::int64_t>({}, read).value() == 7,
            "escaping cancellation committed graph state");
    using SourceResult = Result<std::int64_t>;
    const auto ordinary = child->invoke<SourceResult>({}, [&](ObjectExecution& execution) {
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 9; });
        return SourceResult::Failure("source.domain_refusal");
    });
    require(ordinary.is_success() && ordinary.value().is_failure() &&
            root->invoke<std::int64_t>({}, read).value() == 9,
            "ordinary returned Result semantics changed at native boundary");

    CancellationSource before;
    before.cancel();
    std::size_t calls = 0;
    require(root->invoke<int>(before.token(), [&](ObjectExecution&) { ++calls; return 1; }).is_cancelled() && calls == 0,
            "pre-cancelled invocation executed source");
    CancellationSource during;
    const auto cancelled = child->invoke<void>(during.token(), [&](ObjectExecution& execution) {
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 44; });
        during.cancel();
        execution.budget.step();
    });
    require(cancelled.is_cancelled() && root->invoke<std::int64_t>({}, read).value() == 9,
            "cooperative cancellation did not roll back");
    CancellationSource after;
    const auto authoritative = child->invoke<int>(after.token(), [&](ObjectExecution& execution) {
        execution.transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 10; });
        after.cancel();
        return 10;
    });
    require(authoritative.is_success() && authoritative.value() == 10 && root->invoke<std::int64_t>({}, read).value() == 10,
            "late cancellation relabelled authoritative success");

    auto otherOwner = ExecutionOwner::Create().value();
    auto foreign = otherOwner->rootExecution().value();
    const auto foreignReturn = foreign->invoke<Counter>({}, [&](ObjectExecution&) { return counter; });
    require(foreignReturn.is_failure(), "foreign object escaped return-domain validation");
    require(otherOwner->createScope(child).error() == "di.execution_foreign_owner", "foreign parent context accepted");
    require(otherOwner->close().is_success(), "foreign owner close failed");
    require(child->close().is_success() && child->close().is_success(), "child close not idempotent");
    require(child->checkOpen().is_failure(), "closed child admitted cache access");
    require(root->invoke<std::int64_t>({}, read).value() == 10, "child close incorrectly revoked whole owner domain");
    require(owner->close().is_success() && owner->close().is_success(), "root close failed");
    bool expired = false;
    try {
        counter.require();
    } catch (const ExecutionFailure&) {
        expired = true;
    }
    require(expired && root->invoke<int>({}, [](ObjectExecution&) { return 0; }).is_failure(),
            "closed root retained usable execution/domain");
}

void guardsAndLifetimes()
{
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto child = owner->createScope(root).value();
    const auto guarded = root->invoke<int>({}, [&](ObjectExecution& execution) {
        auto hostCall = execution.scope.enterSynchronousHostCall();
        require(child->checkOpen().error().find("reentry") != std::string::npos, "cache-hit native host reentry accepted");
        require(child->invoke<int>({}, [](ObjectExecution&) { return 1; }).is_failure(), "source native host reentry accepted");
        require(owner->rootExecution().is_failure() && owner->createScope(root).is_failure() &&
                owner->close().is_failure() && child->close().is_failure(), "host callback changed owner lifecycle");
        return 1;
    });
    require(guarded.is_success() && root->checkOpen().is_success(), "native callback guard failed to release");
    const auto nested = root->invoke<int>({}, [&](ObjectExecution&) {
        require(child->checkOpen().error() == "di.execution_reentry", "nested native invocation read cache");
        require(owner->close().error() == "di.execution_reentry", "active callback mutated owner close state");
        require(root->close().error() == "di.execution_reentry", "active callback mutated root close state");
        require(child->close().error() == "di.execution_reentry", "active callback mutated child close state");
        return 2;
    });
    require(nested.is_success(), "nested guard corrupted outer invocation");
    require(root->checkOpen().is_success() && child->checkOpen().is_success(), "rejected close reentry left a Closing context");
    require(child->invoke<int>({}, [](ObjectExecution&) { return 3; }).value() == 3, "rejected close reentry poisoned later source work");
    bool refused = false;
    std::thread worker([&] {
        refused = root->checkOpen().is_failure() && owner->createScope(root).is_failure() &&
            owner->rootExecution().is_failure() && child->close().is_failure() && owner->close().is_failure();
    });
    worker.join();
    require(refused && root->checkOpen().is_success(), "wrong-owner operations mutated execution");

    auto grandchild = owner->createScope(child).value();
    require(child->close().is_success(), "parent did not recursively close children");
    require(grandchild->checkOpen().error() == "di.scope_closed", "closed ancestor admitted descendants");
    require(grandchild->close().is_success() && child->close().is_success(), "child could not finish under Closing ancestor");
    auto remaining = owner->createScope(root).value();
    require(owner->close().is_success(), "owner did not recursively close active children");
    require(remaining->checkOpen().is_failure() && owner->createScope(root).is_failure(), "Closing owner admitted children");
    require(remaining->close().is_success() && owner->close().is_success(), "owner could not finish after children");
    std::weak_ptr<ExecutionOwner> lifetime = owner;
    owner.reset();
    require(!lifetime.expired(), "context did not retain execution owner lifetime");
    root.reset();
    child.reset();
    grandchild.reset();
    remaining.reset();
    require(lifetime.expired(), "owner retained wrapper cycle");
}

void admissionBounds()
{
    require(ExecutionOwner::Create({}, {0, 1}).is_failure(), "invalid root limits accepted");
    TaskOptions invalidTasks;
    invalidTasks.capacity = 0;
    require(ExecutionOwner::Create(invalidTasks).is_failure(), "invalid task capacity accepted");
    auto owner = ExecutionOwner::Create({}, {2, 2}).value();
    auto root = owner->rootExecution().value();
    require(owner->createScope(root, {0, 1}).is_failure(), "invalid child limits accepted");
    auto first = owner->createScope(root).value();
    auto second = owner->createScope(first).value();
    require(owner->createScope(root).error() == "di.activity_limit", "descendant aggregate admission was unbounded");
    require(second->close().is_success() && first->close().is_success(), "bounded children could not finish");
    require(owner->close().is_success(), "failed child admission leaked a parent ticket");
}
}

int main()
{
    try {
        sharedGraphAndTransactions();
        guardsAndLifetimes();
        admissionBounds();
        std::cout << "PASS " << checks << " shared DI execution assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
