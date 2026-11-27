// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Synchronization.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
using namespace synchronization_models;
using IntResult = pipelang::model::Result<std::int64_t, std::string>;
using IntTask = pipelang::model::Task<IntResult>;

pipelang::model::List<std::int64_t> input() {
    auto values = pipelang::model::List<std::int64_t>::Empty();
    for (std::int64_t index = 0; index < 8; ++index)
        values.add(index);
    return values;
}

template<class Model, class Predicate>
void pumpUntil(Model& model, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

template<class Model, class Task>
void finish(Model& model, const Task& task) {
    pumpUntil(model, [&] { return task.completed(); });
}

void locks(std::size_t degree) {
    SynchronizationModel model({}, {degree});
    assert(model.LockReturn().value() == 3);
    assert(model.LockLoop().value() == 2);
    const auto before = model.snapshot();
    assert(model.LockFault().is_failure());
    assert(model.snapshot() == before);
    // A worker runs on another thread, so successful completion also detects a
    // leaked owner-thread acquisition that same-thread reentry would conceal.
    assert(model.ParallelLocked(input()).value() == 16);
    assert(model.LockBudget().is_failure());
    assert(model.snapshot() == before);
    assert(model.ParallelLocked(input()).value() == 16);
    assert(model.LocalSemaphore().value());
    assert(model.ExplicitLock().value() == 4);
    assert(model.SimpleWait().value() == 1);
    assert(model.InvalidCreate().is_failure());
    auto alias = model.AsyncAlias();
    finish(model, alias);
    assert(alias.result().is_success() && alias.result().value());
    assert(model.SynchronousWorkerWait(input()).value() == 8);
}

void waitersAndCleanup() {
    SynchronizationModel model({}, {4});
    auto first = model.Work(11, {});
    auto second = model.Work(22, {});
    assert(!first.completed() && !second.completed() && model.pending() == 2);
    assert(model.EnteredCount().value() == 0);
    assert(model.ReleasePermit(1).value() == 0);
    pumpUntil(model, [&] { return model.EnteredCount().value() == 1; });
    assert(model.PermitCount().value() == 0 && model.CleanedCount().value() == 0);
    model.FinishOne();
    pumpUntil(model, [&] { return model.EnteredCount().value() == 2; });
    assert(model.CleanedCount().value() == 1);
    model.FinishOne();
    finish(model, first);
    finish(model, second);
    assert(first.result().is_success() && first.result().value() == 11);
    assert(second.result().is_success() && second.result().value() == 22);
    assert(model.CleanedCount().value() == 2);
    assert(model.PermitCount().value() == 1 && model.snapshot().marker == 2);
}

void cancellationAndTimeouts() {
    {
        SynchronizationModel model({}, {2});
        auto waiting = model.ObserveCancelledWait({});
        assert(!waiting.completed());
        assert(model.EditMarker(47).is_success());
        waiting.cancel();
        finish(model, waiting);
        assert(waiting.result().is_cancelled());
        assert(model.CleanupObserved().value() == 47);
        assert(model.snapshot().marker == 47);
    }
    {
        SynchronizationModel model({}, {2});
        auto fault = model.AcquiredFault();
        assert(!fault.completed());
        model.ReleasePermit(1);
        finish(model, fault);
        assert(fault.result().is_failure() && fault.result().error() == "acquired source fault");
        assert(model.CleanedCount().value() == 1 && model.PermitCount().value() == 1);
        assert(model.ReleasePermit(0).is_failure());
        assert(model.ReleasePermit(4096).is_failure());
        assert(model.PermitCount().value() == 1);
        auto invalid = model.Timed(-2, {});
        finish(model, invalid);
        assert(invalid.result().is_failure());
        auto excessive = model.Timed(60001, {});
        finish(model, excessive);
        assert(excessive.result().is_failure());
    }
    {
        SynchronizationModel model({}, {2});
        model.ReleasePermit(1);
        pipelang::model::CancellationSource source;
        source.cancel();
        auto cancelled = model.Timed(0, source.token());
        finish(model, cancelled);
        assert(cancelled.result().is_cancelled());
        assert(model.PermitCount().value() == 1);
        auto granted = model.Timed(0, {});
        finish(model, granted);
        assert(granted.result().is_success() && granted.result().value());
        assert(model.PermitCount().value() == 0);
        auto timeout = model.Timed(1, {});
        finish(model, timeout);
        assert(timeout.result().is_success() && !timeout.result().value());
    }
    {
        SynchronizationModel model({}, {2});
        auto waiting = model.Timed(-1, {});
        assert(!waiting.completed());
        waiting.cancel();
        finish(model, waiting);
        assert(waiting.result().is_cancelled());
        model.ReleasePermit(1);
        assert(model.PermitCount().value() == 1);
    }
    {
        SynchronizationModel model({}, {2});
        auto granted = model.Timed(-1, {});
        assert(!granted.completed());
        // Release only makes a permit available. Cancellation before the owner
        // polls readiness must win without consuming that available permit.
        model.ReleasePermit(1);
        granted.cancel();
        finish(model, granted);
        assert(granted.result().is_cancelled());
        assert(model.PermitCount().value() == 1);
    }
    {
        SynchronizationModel model({}, {2});
        auto task = model.Work(33, {});
        model.ReleasePermit(1);
        pumpUntil(model, [&] { return model.EnteredCount().value() == 1; });
        task.cancel();
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(model.CleanedCount().value() == 1);
        assert(model.PermitCount().value() == 1 && model.snapshot().marker == 0);
    }
}

void segmentBudgets() {
    SynchronizationModel model({}, {2});
    auto immediate = model.ImmediateBudget();
    finish(model, immediate);
    assert(immediate.result().is_failure());
    assert(immediate.result().error().find("budget") != std::string::npos);
    auto suspended = model.SuspendedBudget({});
    assert(!suspended.completed());
    model.ReleasePermit(1);
    finish(model, suspended);
    assert(suspended.result().is_success() && suspended.result().value() == 10000);
    assert(model.PermitCount().value() == 1);
}

class ImmediateHost final : public ProgressHost {
public:
    IntResult Step() override {
        calls.fetch_add(1);
        return IntResult::Success(7);
    }
    std::atomic<int> calls{0};
};

void hostProgressAndClosing() {
    auto host = std::make_shared<ImmediateHost>();
    std::optional<IntTask> waiting;
    {
        HostProgressModel model(host);
        waiting = model.Waiting();
        assert(!waiting->completed());
        auto probe = model.Probe();
        finish(model, probe);
        assert(probe.result().is_success() && probe.result().value() == 7);
        assert(host->calls.load() == 1);
        assert(!waiting->completed() && model.snapshot().cleanupCount == 0);
        // Destruction must cancel/drain semaphore waiters without blocking the
        // host executor or retaining a continuation into destroyed model state.
    }
    assert(waiting->completed() && waiting->result().is_cancelled());
}

int main() {
    locks(1);
    locks(4);
    waitersAndCleanup();
    cancellationAndTimeouts();
    segmentBudgets();
    hostProgressAndClosing();
}
