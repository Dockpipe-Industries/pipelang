// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/context.hpp>
#include <pipelang/model/object_model.hpp>
#include <pipelang/model/cleanup.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace pipelang::model;

namespace {
using IntResult = Result<std::int64_t>;

struct CounterTag {
    static constexpr std::uint64_t typeId = 1;
    static bool accepts(std::uint64_t value) { return value == typeId; }
};

struct CounterState final : ObjectState {
    std::int64_t value = 1;
    std::unique_ptr<ObjectState> clone() const override {
        return std::make_unique<CounterState>(*this);
    }
    void trace(ObjectTrace&) const override {}
    std::size_t logicalBytes() const override { return sizeof(CounterState); }
    void validate() const override {}
};

struct RootState {
    std::int64_t value = 1;
    friend void validate(const RootState&) {}
};

template<class Callback>
void expectStale(Callback callback) {
    bool rejected = false;
    try {
        callback();
    } catch (const StaleContextCancelled&) {
        rejected = true;
    }
    assert(rejected);
}

ObjectRef<CounterTag> counter(ObjectOwner& objects) {
    GraphTransaction initial(objects.domain());
    auto reference = initial.create<CounterState>(CounterTag{});
    initial.commit();
    return reference;
}

void ownedPublicationCapturesAdmissionAndRejectsRevocation() {
    ContextOwner owner;
    std::int64_t target = 1;
    std::unique_ptr<Transaction<std::int64_t>> retained;
    {
        InvocationContextScope invocation(owner.capture().permit());
        retained = std::make_unique<Transaction<std::int64_t>>(target);
        retained->state() = 2;
    }
    owner.invalidate();
    assert(currentPublicationGuard().is_current());
    expectStale([&] { retained->commit(); });
    assert(target == 1);
    // The captured guard is immutable even after a valid incarnation replaces it.
    owner.replace();
    expectStale([&] { retained->commit(); });
    assert(target == 1);
    Transaction<std::int64_t> ordinary(target);
    ordinary.state() = 3;
    ordinary.commit();
    assert(target == 3);
}

void synchronousHostRevocationPreservesReceiptAndRollsBackState() {
    ContextOwner owner;
    TaskScope tasks;
    std::int64_t target = 1;
    InvocationContextScope invocation(owner.capture().permit());
    Transaction<std::int64_t> transaction(target);
    transaction.state() = 2;
    const auto receipt = tasks.invokeSynchronousHost([&] {
        owner.invalidate();
        return IntResult::Success(17);
    });
    expectStale([&] { transaction.commit(); });
    assert(target == 1);
    assert(receipt.is_success() && receipt.value() == 17);
}

void graphPublicationRejectsStalePreparedAndProvisionalChanges() {
    ObjectOwner objects;
    const auto reference = counter(objects);
    ContextOwner owner;
    {
        InvocationContextScope invocation(owner.capture().permit());
        GraphTransaction transaction(objects.domain());
        transaction.mutate<CounterState>(reference, [](CounterState& state) { state.value = 2; });
        const auto provisional = transaction.create<CounterState>(CounterTag{});
        transaction.prepareCommit();
        owner.invalidate();
        bool publishedRoot = false;
        expectStale([&] {
            transaction.commit([&]() noexcept { publishedRoot = true; });
        });
        assert(!publishedRoot);
        assert(objects.domain()->liveCount() == 2);
        (void)provisional;
    }
    GraphTransaction observed(objects.domain());
    assert(observed.read<CounterState>(reference).value == 1);
    assert(objects.domain()->liveCount() == 1);
}

void combinedRootGraphPublicationIsAtomicWhenHostRevokes() {
    ObjectOwner objects;
    const auto reference = counter(objects);
    RootState root;
    ContextOwner owner;
    TaskScope tasks;
    {
        InvocationContextScope invocation(owner.capture().permit());
        GraphModelTransaction<RootState> transaction(root, objects.domain());
        transaction.state().value = 2;
        transaction.graph().mutate<CounterState>(reference,
            [](CounterState& state) { state.value = 2; });
        tasks.invokeSynchronousHost([&] {
            owner.invalidate();
            return Result<void>::Success();
        });
        expectStale([&] { transaction.commit(); });
    }
    GraphTransaction observed(objects.domain());
    assert(root.value == 1 && observed.read<CounterState>(reference).value == 1);
    {
        InvocationContextScope invocation(owner.replace().permit());
        GraphModelTransaction<RootState> transaction(root, objects.domain());
        transaction.state().value = 3;
        transaction.graph().mutate<CounterState>(reference,
            [](CounterState& state) { state.value = 3; });
        transaction.commit();
    }
    observed.refresh();
    assert(root.value == 3 && observed.read<CounterState>(reference).value == 3);
}

void nestedInvocationsRestoreCapturedPublicationGuards() {
    ContextOwner outer;
    ContextOwner inner;
    std::int64_t target = 1;
    InvocationContextScope outerInvocation(outer.capture().permit());
    Transaction<std::int64_t> outerTransaction(target);
    outerTransaction.state() = 2;
    outer.invalidate();
    {
        InvocationContextScope innerInvocation(inner.capture().permit());
        Transaction<std::int64_t> innerTransaction(target);
        innerTransaction.state() = 3;
        innerTransaction.commit();
    }
    assert(target == 3 && !currentPublicationGuard().is_current());
    expectStale([&] { outerTransaction.commit(); });
    assert(target == 3);
}

struct HostGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;

    IntResult run() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
        // Deliberately ignore cancellation after admission, as a committed
        // external mutation is authoritative despite later view revocation.
        return IntResult::Success(41);
    }

    void waitEntered() {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
    }

    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

Coroutine<IntResult> suspendedRead(TaskScope& tasks, HostGate& host,
    std::int64_t& target, Result<void>& publication, bool& resumedUnderCapture,
    CancellationToken cancellation) {
    Transaction<std::int64_t> transaction(target);
    transaction.state() = 2;
    transaction.commit();
    const auto result = co_await tasks.host<IntResult>([&host] { return host.run(); }, cancellation);
    resumedUnderCapture = currentPermit().is_bound() && !currentPermit().is_current();
    transaction.refresh();
    transaction.state() = 3;
    try {
        transaction.commit();
        publication = Result<void>::Success();
    } catch (const StaleContextCancelled& error) {
        publication = Result<void>::Cancelled(error.what());
    }
    // A new transaction after suspension must inherit the same stale capture,
    // even if the caller is currently running under a different valid context.
    Transaction<std::int64_t> afterAwait(target);
    afterAwait.state() = 4;
    expectStale([&] { afterAwait.commit(); });
    co_return result;
}

void awaitResumptionsKeepOldAuthorityAndAuthoritativeReceipts() {
    ContextOwner owner;
    ContextOwner unrelated;
    TaskScope tasks;
    HostGate host;
    std::int64_t target = 1;
    Result<void> publication;
    bool resumedUnderCapture = false;
    std::optional<Task<IntResult>> task;
    {
        InvocationContextScope invocation(owner.capture().permit());
        task = tasks.start<IntResult>([&](CancellationToken cancellation) {
            return suspendedRead(tasks, host, target, publication, resumedUnderCapture, cancellation);
        });
    }
    assert(target == 2 && !currentPermit().is_bound());
    host.waitEntered();
    owner.replace();
    host.release();
    {
        InvocationContextScope unrelatedInvocation(unrelated.capture().permit());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!task->completed()) {
            assert(std::chrono::steady_clock::now() < deadline);
            tasks.poll();
            std::this_thread::yield();
        }
        assert(currentPermit().is_current());
    }
    assert(resumedUnderCapture && target == 2 && publication.is_cancelled());
    assert(task->result().is_success() && task->result().value() == 41);
    assert(tasks.pending() == 0 && !currentPermit().is_bound());
}

Coroutine<IntResult> cancelledPoll(TaskScope& tasks, std::int64_t& target,
    bool& resumedUnderCapture, CancellationToken cancellation) {
    try {
        (void)co_await tasks.pollUntil([] { return std::optional<bool>{}; }, cancellation);
    } catch (const OperationCancelled&) {
        resumedUnderCapture = !currentPermit().is_current();
        Transaction<std::int64_t> transaction(target);
        transaction.state() = 2;
        expectStale([&] { transaction.commit(); });
        co_return IntResult::Cancelled("read.cancelled");
    }
    co_return IntResult::Failure("unexpected completion");
}

void pollingResumptionAndStaleAdmissionAreGuarded() {
    ContextOwner owner;
    TaskScope tasks;
    std::int64_t target = 1;
    bool resumedUnderCapture = false;
    std::optional<Task<IntResult>> task;
    const auto lease = owner.capture();
    {
        InvocationContextScope invocation(lease.permit());
        task = tasks.start<IntResult>([&](CancellationToken cancellation) {
            return cancelledPoll(tasks, target, resumedUnderCapture, cancellation);
        });
    }
    owner.invalidate();
    tasks.poll();
    assert(task->completed() && task->result().is_cancelled());
    assert(resumedUnderCapture && target == 1);
    bool factoryCalled = false;
    {
        InvocationContextScope invocation(lease.permit());
        const auto rejected = tasks.start<IntResult>([&](CancellationToken cancellation) {
            factoryCalled = true;
            return cancelledPoll(tasks, target, resumedUnderCapture, cancellation);
        });
        assert(rejected.completed() && rejected.result().is_cancelled());
    }
    assert(!factoryCalled && tasks.pending() == 0);
}

Coroutine<IntResult> returnWithRevokedCleanup(TaskScope& tasks, ContextOwner& owner,
    std::int64_t& target, int& cleanups) {
    Transaction<std::int64_t> transaction(target);
    CommitOnReturn commit(transaction, [&] { return tasks.abandoningFrames(); });
    Finally cleanup([&] { ++cleanups; }, [&] { return tasks.abandoningFrames(); });
    transaction.state() = 2;
    owner.invalidate();
    commit.arm();
    co_return IntResult::Success(7);
}

Coroutine<IntResult> abandonWithCleanup(TaskScope& tasks, std::int64_t& target, int& cleanups) {
    Transaction<std::int64_t> transaction(target);
    CommitOnReturn commit(transaction, [&] { return tasks.abandoningFrames(); });
    Finally cleanup([&] {
        ++cleanups;
        throw ExecutionFailure("cleanup during frame destruction");
    }, [&] { return tasks.abandoningFrames(); });
    transaction.state() = 2;
    commit.arm();
    co_await std::suspend_always{};
    co_return IntResult::Success(7);
}

void cleanupCommitsRejectNormallyAndSkipUnwindingOrAbandonment() {
    ContextOwner owner;
    std::int64_t target = 1;
    int cleanups = 0;
    {
        TaskScope tasks;
        InvocationContextScope invocation(owner.capture().permit());
        const auto task = tasks.start<IntResult>([&](CancellationToken) {
            return returnWithRevokedCleanup(tasks, owner, target, cleanups);
        });
        assert(task.completed() && task.result().is_cancelled());
        assert(cleanups == 1 && target == 1);
    }
    {
        InvocationContextScope invocation(owner.replace().permit());
        Transaction<std::int64_t> transaction(target);
        bool preserved = false;
        try {
            CommitOnReturn commit(transaction, [] { return false; });
            commit.arm();
            owner.invalidate();
            throw ExecutionFailure("primary failure");
        } catch (const ExecutionFailure& error) {
            preserved = std::string(error.what()) == "primary failure";
        }
        assert(preserved && target == 1);
    }
    {
        TaskScope tasks;
        InvocationContextScope invocation(owner.replace().permit());
        const auto task = tasks.start<IntResult>([&](CancellationToken) {
            return abandonWithCleanup(tasks, target, cleanups);
        });
        assert(!task.completed());
        owner.invalidate();
        tasks.shutdown();
        assert(task.completed() && task.result().is_cancelled());
        assert(cleanups == 2 && target == 1);
    }
}

void retainedSourceOwnersRevokeWithTheirTaskScope() {
    std::shared_ptr<ContextOwner> retained;
    ContextLease retainedLease;
    int notifications = 0;
    RevocationSubscription observer;
    {
        TaskScope tasks;
        retained = tasks.createContextOwner();
        retainedLease = retained->capture();
        observer = retainedLease.observeRevocation([&] { ++notifications; });
        assert(retainedLease.is_current());
    }
    assert(retained && !retainedLease.is_current() && notifications == 1);
    assert(retainedLease.cancellation().is_cancelled());
    assert(!retained->replace().is_current());

    ContextOwner external;
    TaskScope tasks;
    std::shared_ptr<ContextOwner> first;
    std::shared_ptr<ContextOwner> second;
    {
        InvocationContextScope invocation(external.capture().permit());
        first = tasks.createContextOwner();
        second = tasks.createContextOwner();
    }
    assert(first->capture().is_current() && second->capture().is_current());
    assert(first->rootIdentity() == external.rootIdentity());
    const auto combined = InvocationPermit::combine(first->capture().permit(), second->capture().permit());
    assert(combined.is_current());
    tasks.shutdown();
    assert(!first->capture().is_current() && !second->capture().is_current());
    assert(!combined.is_current() && external.capture().is_current());
    bool closed = false;
    try {
        (void)tasks.createContextOwner();
    } catch (const ExecutionFailure& error) {
        closed = std::string(error.what()) == "task owner is closing";
    }
    assert(closed);
}

void lazySourceOwnersInheritScopeLifetimeAndCurrentOperationSeparately() {
    ContextOwner root;
    OperationLane lane(root);
    std::shared_ptr<ContextOwner> first;
    std::shared_ptr<ContextOwner> second;
    {
        // Deliberately born unbound: first operation A supplies root identity
        // without becoming the cancellation lifetime of this model's scope.
        TaskScope tasks;
        const auto firstAdmission = lane.begin(root.capture());
        assert(firstAdmission.is_success());
        const auto firstOperation = firstAdmission.value();
        {
            InvocationContextScope operation(firstOperation.permit());
            first = tasks.createContextOwner();
            assert(first->capture().is_current());
            assert(first->rootIdentity() == root.rootIdentity());
        }
        assert(firstOperation.complete().is_success());
        assert(!first->capture().is_current());
        const auto secondAdmission = lane.begin(root.capture());
        assert(secondAdmission.is_success());
        {
            InvocationContextScope operation(secondAdmission.value().permit());
            second = tasks.createContextOwner();
            assert(second->capture().is_current());
        }
        assert(!first->capture().is_current());
        ContextOwner unrelated;
        bool rejected = false;
        {
            InvocationContextScope foreignInvocation(unrelated.capture().permit());
            try {
                (void)tasks.createContextOwner();
            } catch (const ExecutionFailure& error) {
                rejected = std::string(error.what()) == "context.foreign_root";
            }
        }
        assert(rejected && second->capture().is_current());
    }
    assert(!first->capture().is_current() && !second->capture().is_current());
    assert(root.capture().is_current());
}

void sourceScopeBirthContextCannotBeRefreshedByLaterOperations() {
    ContextOwner root;
    std::unique_ptr<TaskScope> tasks;
    std::shared_ptr<ContextOwner> retained;
    {
        InvocationContextScope birth(root.capture().permit());
        tasks = std::make_unique<TaskScope>();
        retained = tasks->createContextOwner();
    }
    assert(retained->capture().is_current());
    root.replace();
    assert(!retained->capture().is_current());
    {
        InvocationContextScope newer(root.capture().permit());
        expectStale([&] { (void)tasks->createContextOwner(); });
    }
    assert(root.capture().is_current());
}
}

int main() {
    ownedPublicationCapturesAdmissionAndRejectsRevocation();
    synchronousHostRevocationPreservesReceiptAndRollsBackState();
    graphPublicationRejectsStalePreparedAndProvisionalChanges();
    combinedRootGraphPublicationIsAtomicWhenHostRevokes();
    nestedInvocationsRestoreCapturedPublicationGuards();
    awaitResumptionsKeepOldAuthorityAndAuthoritativeReceipts();
    pollingResumptionAndStaleAdmissionAreGuarded();
    cleanupCommitsRejectNormallyAndSkipUnwindingOrAbandonment();
    retainedSourceOwnersRevokeWithTheirTaskScope();
    lazySourceOwnersInheritScopeLifetimeAndCurrentOperationSeparately();
    sourceScopeBirthContextCannotBeRefreshedByLaterOperations();
}
