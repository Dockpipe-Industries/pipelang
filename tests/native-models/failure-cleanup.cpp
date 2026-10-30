// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "FailureCleanup.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
using namespace pipelang_models;
using pipelang::model::CancellationToken;
using IntResult = pipelang::model::Result<std::int64_t, std::string>;
using IntTask = pipelang::model::Task<IntResult>;

class ControlledCleanupHost final : public CleanupHost {
public:
    IntResult Work(std::int64_t value, CancellationToken token) override {
        std::unique_lock lock(mutex);
        ++calls;
        entered = true;
        changed.notify_all();
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
        committed = true;
        changed.notify_all();
        // A successful host return remains authoritative after cancellation.
        (void)token;
        return IntResult::Success(value);
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
    void waitCommitted() {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return committed; }));
    }
    int callCount() {
        std::lock_guard lock(mutex);
        return calls;
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool committed = false;
    int calls = 0;
};

template<class Task>
void finish(FailureCleanup& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void synchronous() {
    auto host = std::make_shared<ControlledCleanupHost>();
    FailureCleanup model(host);
    assert(model.OwnedError("original owned text").value() == "original owned text");
    assert(model.ReturnOwnedLocal().value() == "owned before finally");
    assert(model.snapshot().detail == "owned before finally");
    assert(model.PureOwnedReturn() == "pure original");
    assert(model.CatchPartial().value() == 17);
    assert(!model.snapshot().detail.empty());
    assert(model.CatchArithmetic().value() == "integer division by zero");
    const auto before = model.snapshot();
    auto failure = model.Uncaught();
    assert(failure.is_failure() && failure.error() == "primary fault");
    assert(model.snapshot() == before);
    assert(model.ReturnBeforeCleanup().value() == 5);
    assert(model.snapshot().count == 15);
    for (bool branch : {false, true}) {
        assert(model.Nested(branch).value() == (branch ? 1 : 2));
        assert(model.snapshot().count == 12);
    }
    assert(model.LoopExits().value() == 24);
    assert(model.PrimaryWins().value() == "original");
    assert(model.Rethrow().value() == "rethrow original");
    assert(model.snapshot().detail == "rethrow original");
    const auto saved = model.snapshot();
    auto cleanup = model.CleanupReplacesReturn();
    assert(cleanup.is_failure() && cleanup.error() == "cleanup fault");
    assert(model.snapshot() == saved);
    for (bool cancelled : {false, true}) {
        const auto result = model.TypedOutcome(cancelled);
        assert(cancelled ? result.is_cancelled() : result.is_failure());
        assert(result.error() == (cancelled ? "typed cancel" : "typed failure"));
        assert(model.snapshot().count == 8);
    }
    const auto priorLimit = model.snapshot();
    const auto limit = model.NormalBudgetIsNotCaught();
    assert(limit.is_failure());
    assert(limit.error().find("budget") != std::string::npos ||
           limit.error().find("limit") != std::string::npos);
    assert(limit.error() != "incorrectly caught limit");
    assert(limit.error() != "secondary cleanup fault");
    assert(model.snapshot() == priorLimit);
    const auto reserve = model.SharedCleanupReserve();
    assert(reserve.is_failure());
    assert(reserve.error().find("budget") != std::string::npos ||
           reserve.error().find("limit") != std::string::npos);
    assert(model.snapshot() == priorLimit);
    assert(model.LocalCleanupLoop().value() == 4);
    auto child = model.ChildOutcome();
    finish(model, child);
    assert(child.result().is_failure() && child.result().error() == "typed child failure");
    assert(model.snapshot().count == 11);
}

void asynchronous() {
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        FailureCleanup model(host);
        auto task = model.RecoverReturnCleanup({});
        host->waitEntered();
        assert(!task.completed());
        assert(model.pending() == 1);
        assert(model.snapshot().count == 11);
        assert(model.snapshot().detail.empty());
        host->release();
        finish(model, task);
        assert(task.result().is_success() && task.result().value() == 9);
        assert(model.snapshot().count == 112);
        assert(model.snapshot().detail == "return cleanup recovery");
        assert(model.pending() == 0);
    }
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        FailureCleanup model(host);
        auto task = model.TryAwait({}, false);
        host->waitEntered();
        assert(model.snapshot().count == 11); // finally has not run at suspension.
        assert(model.Edit(50).is_success());
        task.cancel();
        host->release();
        finish(model, task);
        assert(task.result().is_success() && task.result().value() == 7);
        assert(model.snapshot().count == 151);
    }
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        FailureCleanup model(host);
        auto task = model.CatchAwait({});
        host->waitEntered();
        assert(model.snapshot().count == 0);
        assert(model.snapshot().detail.empty());
        host->release();
        finish(model, task);
        assert(task.result().is_success() && task.result().value() == 8);
        assert(model.snapshot().detail == "owned across catch suspension");
        assert(model.snapshot().count == 100);
    }
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        FailureCleanup model(host);
        auto task = model.TryAwait({}, true);
        host->waitEntered();
        assert(model.Edit(50).is_success());
        host->release();
        finish(model, task);
        assert(task.result().is_failure() && task.result().error() == "async cleanup fault");
        assert(model.snapshot().count == 50); // Last segment, including cleanup, rolled back.
    }
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        FailureCleanup model(host);
        pipelang::model::CancellationSource source;
        source.cancel();
        auto task = model.TryAwait(source.token(), false);
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(host->callCount() == 0);
        assert(model.snapshot().count == 112);
    }
    {
        auto host = std::make_shared<ControlledCleanupHost>();
        std::optional<IntTask> retained;
        {
            FailureCleanup model(host);
            retained = model.TryAwait({}, true);
            host->waitEntered();
            host->release();
            host->waitCommitted();
            // Drain must resume source finally. Failure proves cleanup executed
            // despite no explicit poll before owner teardown.
        }
        assert(retained->completed());
        assert(retained->result().is_failure());
        assert(retained->result().error() == "async cleanup fault");
    }
}

struct AbandonState {
    int value = 0;
};

pipelang::model::Coroutine<IntResult> dormant(
    pipelang::model::TaskScope& scope, AbandonState& state, bool& cleaned) {
    pipelang::model::Transaction<AbandonState> transaction(state);
    pipelang::model::CommitOnReturn commit(transaction,
        [&scope]() noexcept { return scope.abandoningFrames(); });
    commit.arm();
    [[maybe_unused]] auto cleanup = pipelang::model::Finally(
        [&] {
            cleaned = true;
            transaction.state().value = 77;
            throw pipelang::model::ExecutionFailure("abandoned cleanup fault");
        },
        [&scope]() noexcept { return scope.abandoningFrames(); });
    co_await std::suspend_always{};
    co_return IntResult::Success(1);
}

void runtimeOwnership() {
    pipelang::model::EvaluationBudget budget;
    bool exhausted = false;
    try {
        for (std::size_t step = 0; step <= pipelang::model::EvaluationBudget::maximumSteps; ++step)
            budget.step();
    } catch (const pipelang::model::EvaluationLimitFailure& error) {
        exhausted = true;
        assert(std::string(error.what()).find("budget") != std::string::npos);
    }
    assert(exhausted);
    {
        auto reserve = budget.cleanup();
        budget.step(); // The independent reserve remains available.
    }
    bool cleaned = false;
    AbandonState state;
    std::optional<IntTask> retained;
    {
        pipelang::model::TaskScope scope;
        retained = scope.start<IntResult>([&](CancellationToken) {
            return dormant(scope, state, cleaned);
        });
        assert(!retained->completed());
    }
    assert(cleaned);
    assert(retained->completed() && retained->result().is_cancelled());
    assert(state.value == 0); // Forced abandonment never commits an armed return.

    auto errors = pipelang::model::List<pipelang::model::Optional<pipelang::model::Error>>::Empty();
    bool capped = false;
    try {
        errors.add(pipelang::model::Optional<pipelang::model::Error>::Some(
            pipelang::model::makeError(std::string(1024 * 1024, 'x'))));
    } catch (const pipelang::model::ExecutionFailure&) {
        capped = true;
    }
    assert(capped && errors.count() == 0);
}

int main() {
    runtimeOwnership();
    synchronous();
    asynchronous();
}
