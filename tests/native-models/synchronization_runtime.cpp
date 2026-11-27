// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/synchronization.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <latch>
#include <optional>
#include <string>
#include <thread>

using namespace pipelang::model;
using IntResult = Result<std::int64_t, std::string>;

namespace {

template<class Function>
void expectFailure(Function function) {
    bool failed = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

struct CleanupMark {
    int& cleaned;
    ~CleanupMark() {
        ++cleaned;
    }
};

Coroutine<IntResult> waiting(TaskScope& scope, SemaphoreSlim semaphore,
                             CancellationToken cancellation, int& cleaned,
                             std::int64_t timeoutMs = -1) {
    CleanupMark mark{cleaned};
    const bool acquired = co_await semaphore.waitAsync(scope, timeoutMs, cancellation);
    co_return IntResult::Success(acquired ? 1 : 0);
}

Coroutine<IntResult> hostJob(TaskScope& scope, std::latch& entered) {
    co_return co_await scope.host<IntResult>([&] {
        entered.count_down();
        return IntResult::Success(7);
    }, {});
}

Coroutine<IntResult> twoWaits(TaskScope& scope, SemaphoreSlim semaphore, int& completed) {
    bool first = co_await semaphore.waitAsync(scope);
    assert(first);
    ++completed;
    bool second = co_await semaphore.waitAsync(scope);
    assert(second);
    ++completed;
    co_return IntResult::Success(completed);
}

template<class T>
void finish(TaskScope& scope, const Task<T>& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        scope.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void locks() {
    Lock mutex;
    auto shared = mutex;
    {
        EvaluationBudget budget;
        auto outer = mutex.acquire(budget);
        auto inner = shared.acquire(budget);
        auto moved = std::move(inner);
        (void)outer;
        (void)moved;
    }
    std::atomic<int> inside{0};
    int count = 0;
    std::array<std::jthread, 4> workers;
    for (auto& worker : workers) {
        worker = std::jthread([&] {
            EvaluationBudget budget;
            for (int index = 0; index < 100; ++index) {
                auto guard = shared.acquire(budget);
                assert(inside.fetch_add(1) == 0);
                ++count;
                assert(inside.fetch_sub(1) == 1);
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(count == 400 && inside.load() == 0);

    auto returning = [&] {
        EvaluationBudget budget;
        auto guard = mutex.acquire(budget);
        return 7;
    };
    assert(returning() == 7);
    try {
        EvaluationBudget budget;
        auto guard = mutex.acquire(budget);
        throw ExecutionFailure("protected fault");
    } catch (const ExecutionFailure&) {
    }
    std::jthread afterFault([&] {
        EvaluationBudget budget;
        auto guard = mutex.acquire(budget);
        ++count;
    });
    afterFault.join();
    assert(count == 401);

    CancellationSource cancellation;
    cancellation.cancel();
    bool cancelled = false;
    EvaluationBudget budget;
    try {
        mutex.acquire(budget, cancellation.token());
    } catch (const OperationCancelled&) {
        cancelled = true;
    }
    assert(cancelled);
    {
        auto cleanup = budget.cleanup();
        auto guard = mutex.acquire(budget, cancellation.token());
        (void)cleanup;
        (void)guard;
    }
    auto moved = std::move(shared);
    validate(moved);
    expectFailure([&] { shared.acquire(budget); });
}

void synchronousSemaphore() {
    auto semaphore = SemaphoreSlim::Create(1, 2);
    auto alias = semaphore;
    EvaluationBudget budget;
    assert(semaphore.wait(budget, 0));
    assert(!alias.wait(budget, 0));
    assert(alias.release(2) == 0);
    expectFailure([&] { semaphore.release(); });
    expectFailure([&] { semaphore.release(0); });
    assert(alias.currentCount() == 2);
    CancellationSource cancellation;
    cancellation.cancel();
    bool cancelled = false;
    try {
        alias.wait(budget, 0, cancellation.token());
    } catch (const OperationCancelled&) {
        cancelled = true;
    }
    assert(cancelled && alias.currentCount() == 2);
    {
        auto cleanup = budget.cleanup();
        assert(alias.wait(budget, 0, cancellation.token()));
        (void)cleanup;
    }
    assert(alias.wait(budget, 0));
    assert(!alias.wait(budget, 1));

    EvaluationBudget almostExhausted;
    for (std::size_t index = 0; index < EvaluationBudget::maximumSteps - 1; ++index)
        almostExhausted.step();
    expectFailure([&] { semaphore.wait(almostExhausted); });
    assert(semaphore.currentCount() == 0);
    expectFailure([&] { SemaphoreSlim::Create(-1); });
    expectFailure([&] { SemaphoreSlim::Create(2, 1); });
    expectFailure([&] { SemaphoreSlim::Create(0, 4097); });
    expectFailure([&] { semaphore.wait(budget, -2); });
    expectFailure([&] { semaphore.wait(budget, 60001); });
    auto moved = std::move(alias);
    validate(moved);
    expectFailure([&] { alias.currentCount(); });
}

void asynchronousSemaphore() {
    TaskScope scope;
    SemaphoreSlim semaphore;
    int cleaned = 0;
    auto task = scope.start<IntResult>([&](CancellationToken cancellation) {
        return waiting(scope, semaphore, cancellation, cleaned);
    });
    assert(!task.completed());
    std::latch entered(1);
    auto host = scope.start<IntResult>([&](CancellationToken) {
        return hostJob(scope, entered);
    });
    entered.wait();
    finish(scope, host);
    assert(host.result().value() == 7 && !task.completed());
    semaphore.release();
    finish(scope, task);
    assert(task.result().value() == 1 && semaphore.currentCount() == 0 && cleaned == 1);

    auto timed = scope.start<IntResult>([&](CancellationToken cancellation) {
        return waiting(scope, semaphore, cancellation, cleaned, 1);
    });
    finish(scope, timed);
    assert(timed.result().is_success() && timed.result().value() == 0);
    auto cancelled = scope.start<IntResult>([&](CancellationToken cancellation) {
        return waiting(scope, semaphore, cancellation, cleaned);
    });
    cancelled.cancel();
    semaphore.release();
    finish(scope, cancelled);
    assert(cancelled.result().is_cancelled() && semaphore.currentCount() == 1);

    CancellationSource source;
    auto immediate = semaphore.waitAsync(scope, 0, source.token());
    assert(immediate.await_ready());
    source.cancel();
    assert(immediate.await_resume());
    assert(!*immediate.suspensionState());

    auto suspended = semaphore.waitAsync(scope);
    assert(!suspended.await_ready());
    assert(suspended.await_suspend(std::noop_coroutine()));
    assert(*suspended.suspensionState());
    semaphore.release();
    assert(scope.poll() == 1);
    assert(suspended.await_resume());
    assert(scope.pending() == 0);
}

void pollingSnapshotAndFailures() {
    TaskScope scope;
    SemaphoreSlim semaphore;
    int completed = 0;
    auto task = scope.start<IntResult>([&](CancellationToken) {
        return twoWaits(scope, semaphore, completed);
    });
    semaphore.release();
    assert(scope.poll() == 1);
    assert(completed == 1 && !task.completed());
    semaphore.release();
    assert(scope.poll() == 1);
    assert(completed == 2 && task.result().value() == 2);

    auto failed = scope.pollUntil([]() -> std::optional<bool> {
        throw ExecutionFailure("predicate fault");
    });
    assert(failed.await_ready());
    expectFailure([&] { failed.await_resume(); });

    CancellationSource source;
    auto granted = scope.pollUntil([&]() -> std::optional<bool> {
        source.cancel();
        return true;
    }, source.token());
    assert(granted.await_ready() && granted.await_resume());
}

void frameDestructionAndCapacity() {
    TaskScope scope;
    SemaphoreSlim semaphore;
    int cleaned = 0;
    {
        auto child = waiting(scope, semaphore, {}, cleaned);
        child.handle.resume();
        assert(!child.handle.done());
    }
    assert(cleaned == 1);
    semaphore.release();
    assert(scope.poll() == 0 && semaphore.currentCount() == 1);
    EvaluationBudget budget;
    assert(semaphore.wait(budget, 0));

    std::array<std::optional<Coroutine<IntResult>>, 33> children;
    for (auto& child : children) {
        child.emplace(waiting(scope, semaphore, {}, cleaned));
        child->handle.resume();
    }
    for (std::size_t index = 0; index < 32; ++index)
        assert(!children[index]->handle.done());
    assert(children[32]->handle.done());
    assert(children[32]->handle.promise().state->result->is_failure());
    for (auto& child : children)
        child.reset();
    semaphore.release();
    assert(scope.poll() == 0 && semaphore.currentCount() == 1);
    assert(cleaned == 34);
}

void teardown() {
    SemaphoreSlim semaphore;
    int cleaned = 0;
    std::optional<Task<IntResult>> retained;
    {
        TaskScope scope;
        retained = scope.start<IntResult>([&](CancellationToken cancellation) {
            return waiting(scope, semaphore, cancellation, cleaned);
        });
        semaphore.release();
        // Scope closure cancels the registered operation without polling its
        // acquisition predicate, even though a permit is now available.
    }
    assert(retained->completed() && retained->result().is_cancelled());
    assert(cleaned == 1 && semaphore.currentCount() == 1);
}

}

int main() {
    locks();
    synchronousSemaphore();
    asynchronousSemaphore();
    pollingSnapshotAndFailures();
    frameDestructionAndCapacity();
    teardown();
}
