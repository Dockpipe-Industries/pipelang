// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/cleanup.hpp>
#include <pipelang/model/source_call.hpp>
#include <pipelang/model/task.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace pipelang::model;
using IntResult = Result<std::int64_t>;
std::size_t assertions = 0;

void require(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}

template<class Predicate>
void pumpUntil(TaskScope& scope, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        scope.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("runtime fixture pump deadline");
        std::this_thread::yield();
    }
}

template<class T>
void finish(TaskScope& scope, const Task<T>& task) {
    pumpUntil(scope, [&] { return task.completed(); });
}

Coroutine<IntResult> cleanupAfterCancellation(TaskScope& scope, CancellationToken cancellation,
    std::atomic<bool>& bodyEntered, std::atomic<int>& cleanupCalls) {
    EvaluationBudget budget;
    AsyncFinallyExit<IntResult> exit;
    try {
        std::function<IntResult()> body = [cancellation, &bodyEntered] {
            bodyEntered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!cancellation.is_cancelled()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return IntResult::Failure("body did not receive cancellation");
                std::this_thread::yield();
            }
            return IntResult::Cancelled("body cancelled");
        };
        auto operation = scope.host<IntResult>(std::move(body), cancellation);
        exit.returning(co_await std::move(operation));
    } catch (...) {
        exit.captureFailure();
    }
    try {
        auto reserve = budget.cleanup();
        auto cleanup = scope.enterCleanup(cancellation);
        budget.step();
        std::function<IntResult()> function = [cancellation, &cleanupCalls] {
            if (cancellation.is_cancelled())
                return IntResult::Failure("normal cancellation leaked into cleanup");
            ++cleanupCalls;
            return IntResult::Success(1);
        };
        auto operation = scope.host<IntResult>(std::move(function), cancellation);
        auto result = co_await std::move(operation);
        if (!result.is_success())
            throw ExecutionFailure(result.error());
    } catch (...) {
        exit.captureCleanupFailure();
    }
    exit.rethrowFailure();
    co_return std::move(exit.returnValue());
}

Coroutine<IntResult> polling(TaskScope& scope, CancellationToken cancellation, bool& ready) {
    (void)co_await scope.pollUntil([&ready]() -> std::optional<bool> {
        if (ready)
            return true;
        return std::nullopt;
    }, cancellation);
    co_return IntResult::Success(1);
}

void drainContracts() {
    {
        TaskScope scope;
        std::atomic<bool> entered = false;
        std::atomic<int> cleaned = 0;
        auto task = scope.start<IntResult>([&](CancellationToken token) {
            return cleanupAfterCancellation(scope, token, entered, cleaned);
        });
        pumpUntil(scope, [&] { return entered.load(); });
        scope.shutdown();
        require(task.completed() && task.result().is_cancelled(), "shutdown did not settle body cancellation");
        require(cleaned.load() == 1, "shutdown skipped or repeated asynchronous cleanup");
        scope.shutdown();
        require(cleaned.load() == 1, "shutdown was not idempotent");
    }
    {
        TaskScope scope;
        auto first = scope.createGroup();
        auto second = scope.createGroup();
        std::atomic<bool> entered = false;
        std::atomic<int> cleaned = 0;
        bool ready = false;
        auto body = scope.tryStart<IntResult>(first, [&](CancellationToken token) {
            return cleanupAfterCancellation(scope, token, entered, cleaned);
        }).value();
        auto sibling = scope.tryStart<IntResult>(second, [&](CancellationToken token) {
            return polling(scope, token, ready);
        }).value();
        pumpUntil(scope, [&] { return entered.load(); });
        scope.drainGroup(first);
        require(body.result().is_cancelled() && cleaned.load() == 1, "group drain did not await cleanup");
        require(!sibling.completed() && scope.pending(second) == 1, "cleanup phase leaked into another group");
        ready = true;
        finish(scope, sibling);
        require(sibling.result().is_success(), "unrelated group cancellation changed");
    }
}

Coroutine<IntResult> exactCleanupBudget(TaskScope& scope, CancellationToken cancellation, std::size_t& consumed) {
    EvaluationBudget budget;
    for (std::size_t index = 0; index < EvaluationBudget::maximumSteps; ++index)
        budget.step();
    auto reserve = budget.cleanup();
    auto cleanup = scope.enterCleanup(cancellation);
    for (std::size_t index = 0; index < EvaluationBudget::maximumSteps; ++index) {
        budget.step();
        ++consumed;
        if (index % 16384 == 0) {
            std::function<IntResult()> function = [] { return IntResult::Success(1); };
            auto operation = scope.host<IntResult>(std::move(function), cancellation);
            auto result = co_await std::move(operation);
            if (!result.is_success())
                throw ExecutionFailure(result.error());
            budget.reset();
        }
    }
    budget.step();
    co_return IntResult::Success(0);
}

Coroutine<IntResult> inheritedTerminal(TaskScope& scope, CancellationToken cancellation, std::size_t& consumed) {
    try {
        auto child = exactCleanupBudget(scope, cancellation, consumed);
        auto result = co_await std::move(child);
        co_return IntResult::Success(result.is_failure() ? 999 : 998);
    } catch (const EvaluationLimitFailure&) {
        throw;
    }
}

void budgetContract() {
    TaskScope scope;
    std::size_t consumed = 0;
    auto task = scope.start<IntResult>([&](CancellationToken cancellation) {
        return inheritedTerminal(scope, cancellation, consumed);
    });
    finish(scope, task);
    require(consumed == 65536, "cleanup reserve did not retain its exact bound across real suspensions");
    require(task.result().is_failure() && task.result().error().find("cleanup evaluation budget") != std::string::npos,
        "awaited terminal cleanup exhaustion became a recoverable child Result");
    SourceCallContext calls;
    for (unsigned depth = 0; depth < 32; ++depth)
        calls = calls.enter();
    bool refused = false;
    try {
        (void)calls.enter();
    } catch (const EvaluationLimitFailure&) {
        refused = true;
    }
    require(refused && calls.depth() == 32, "cleanup capability changed source ancestry bounds");
}

struct Descriptors {
    std::optional<TaskScope::HostAwaitable<IntResult>> host;
    std::optional<TaskScope::PollingAwaitable> poll;
    int hostCalls = 0;
    int predicates = 0;
    bool release = false;
};

Coroutine<IntResult> descriptorOwner(TaskScope& scope, CancellationToken token, Descriptors& descriptors) {
    auto cleanup = scope.enterCleanup(token);
    std::function<IntResult()> function = [&descriptors] {
        ++descriptors.hostCalls;
        return IntResult::Success(1);
    };
    descriptors.host.emplace(scope.host<IntResult>(std::move(function), token));
    descriptors.poll.emplace(scope.pollUntil([&descriptors] {
        ++descriptors.predicates;
        return std::optional<bool>(true);
    }, token));
    (void)co_await scope.pollUntil([&descriptors]() -> std::optional<bool> {
        if (descriptors.release)
            return true;
        return std::nullopt;
    }, token);
    co_return IntResult::Success(1);
}

Coroutine<IntResult> stealHost(Descriptors& descriptors) {
    auto result = co_await std::move(*descriptors.host);
    co_return result;
}

Coroutine<IntResult> stealPolling(Descriptors& descriptors) {
    (void)co_await std::move(*descriptors.poll);
    co_return IntResult::Success(1);
}

Coroutine<IntResult> foreignReady(TaskScope& foreign, int& predicates, bool ready) {
    (void)co_await foreign.pollUntil([&predicates, ready]() -> std::optional<bool> {
        ++predicates;
        if (ready)
            return true;
        return std::nullopt;
    });
    co_return IntResult::Success(1);
}

Coroutine<IntResult> expiredDescriptor(TaskScope& scope, CancellationToken token,
    const std::shared_ptr<TaskGroup>& group, int& calls, bool replacementPhase, bool pollingDescriptor) {
    std::optional<TaskScope::HostAwaitable<IntResult>> host;
    std::optional<TaskScope::PollingAwaitable> poll;
    {
        auto cleanup = scope.enterCleanup(token);
        std::function<IntResult()> function = [&calls] { ++calls; return IntResult::Success(1); };
        host.emplace(scope.host<IntResult>(std::move(function), token));
        poll.emplace(scope.pollUntil([&calls] {
            ++calls;
            return std::optional<bool>(true);
        }, token));
    }
    scope.closeGroup(group);
    if (replacementPhase) {
        auto cleanup = scope.enterCleanup(token);
        if (pollingDescriptor)
            (void)co_await std::move(*poll);
        else
            (void)co_await std::move(*host);
    } else {
        if (pollingDescriptor)
            (void)co_await std::move(*poll);
        else
            (void)co_await std::move(*host);
    }
    co_return IntResult::Success(1);
}

void ownershipContracts() {
    {
        TaskScope scope;
        Descriptors descriptors;
        auto groupA = scope.createGroup();
        auto groupB = scope.createGroup();
        auto owner = scope.tryStart<IntResult>(groupA, [&](CancellationToken token) {
            return descriptorOwner(scope, token, descriptors);
        }).value();
        require(!owner.completed(), "descriptor owner did not suspend");
        auto hostThief = scope.tryStart<IntResult>(groupB, [&](CancellationToken) {
            return stealHost(descriptors);
        }).value();
        auto pollThief = scope.tryStart<IntResult>(groupB, [&](CancellationToken) {
            return stealPolling(descriptors);
        }).value();
        scope.drainGroup(groupB);
        require(hostThief.result().is_failure() && pollThief.result().is_failure(),
            "cleanup descriptors transferred to a sibling continuation");
        require(descriptors.hostCalls == 0 && descriptors.predicates == 0,
            "entry ownership was checked after submission or readiness effects");
        require(!owner.completed() && scope.pending(groupA) == 1,
            "sibling drain destroyed the descriptor-owning frame");
        descriptors.release = true;
        finish(scope, owner);
        descriptors.host.reset();
        descriptors.poll.reset();
        scope.poll();
        require(owner.result().is_success() && scope.pending() == 0,
            "a stale transferred completion survived entry rejection");
    }
    for (const bool replacementPhase : {false, true}) {
        for (const bool pollingDescriptor : {false, true}) {
            TaskScope scope;
            auto group = scope.createGroup();
            int calls = 0;
            auto task = scope.tryStart<IntResult>(group, [&](CancellationToken token) {
                return expiredDescriptor(scope, token, group, calls, replacementPhase, pollingDescriptor);
            }).value();
            scope.drainGroup(group);
            require(task.result().is_failure() && task.result().error().find("expired cleanup phase") != std::string::npos,
                "a retained descriptor revived an expired cleanup phase");
            require(calls == 0, "expired cleanup descriptor performed readiness or host effects");
        }
    }
    for (const bool ready : {false, true}) {
        TaskScope owner;
        TaskScope foreign;
        int predicates = 0;
        auto task = owner.start<IntResult>([&](CancellationToken) {
            return foreignReady(foreign, predicates, ready);
        });
        require(task.result().is_failure() && predicates == 0,
            "cross-scope wait ran a predicate before owner validation");
        foreign.poll();
        require(owner.pending() == 0 && foreign.pending() == 0, "cross-scope rejection retained a continuation");
    }
    {
        TaskScope scope;
        int predicates = 0;
        auto immediate = scope.pollUntil([&] {
            ++predicates;
            return std::optional<bool>(true);
        });
        require(immediate.await_ready() && immediate.await_resume() && predicates == 1,
            "standalone immediate native waiter regressed");
        auto deferred = scope.pollUntil([] { return std::optional<bool>{}; });
        require(!deferred.await_ready() && deferred.await_suspend(std::noop_coroutine()),
            "standalone noop registration regressed");
        scope.shutdown();
        bool cancelled = false;
        try {
            (void)deferred.await_resume();
        } catch (const OperationCancelled&) {
            cancelled = true;
        }
        require(cancelled, "standalone polling registration was not cancelled during shutdown");
    }
}

Coroutine<IntResult> staleCleanup(TaskScope& scope, CancellationToken token, ContextOwner& authority, int& calls) {
    authority.invalidate();
    auto cleanup = scope.enterCleanup(token);
    bool synchronousRefused = false;
    try {
        (void)scope.invokeSynchronousHost([&] { ++calls; return IntResult::Success(1); });
    } catch (const StaleContextCancelled&) {
        synchronousRefused = true;
    }
    if (!synchronousRefused)
        throw ExecutionFailure("stale synchronous cleanup authority admitted");
    std::function<IntResult()> function = [&] { ++calls; return IntResult::Success(2); };
    auto operation = scope.host<IntResult>(std::move(function), token);
    auto result = co_await std::move(operation);
    co_return result;
}

void authorityContract() {
    ContextOwner authority;
    InvocationContextScope invocation(authority.capture().permit());
    TaskScope scope;
    int calls = 0;
    auto task = scope.start<IntResult>([&](CancellationToken token) {
        return staleCleanup(scope, token, authority, calls);
    });
    finish(scope, task);
    require(task.result().is_cancelled() && calls == 0, "cleanup revived stale invocation authority");
}

}

int main() {
    try {
        drainContracts();
        budgetContract();
        ownershipContracts();
        authorityContract();
        std::cout << "async finally: " << assertions << " SDK runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
