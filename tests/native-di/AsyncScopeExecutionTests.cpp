// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/di/ScopeExecution.hpp>
#include <pipelang/model/cleanup.hpp>
#include "ExecutionCounterState.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
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
Coroutine<Result<std::int64_t>> waitLocal(ObjectExecution& execution, CancellationToken token,
                                         bool& ready, std::vector<int>& trace, int marker) {
    Finally finally([&trace, marker] { trace.push_back(marker); }, [&execution] {
        return execution.scope.abandoningFrames();
    });
    (void)co_await execution.scope.pollUntil([&ready]() -> std::optional<bool> {
        if (ready)
            return true;
        return std::nullopt;
    }, token);
    co_return Result<std::int64_t>::Success(marker);
}
Coroutine<Result<std::int64_t>> waitHost(ObjectExecution& execution, CancellationToken token,
                                        std::atomic<bool>& started, std::vector<int>& trace,
                                        bool explicitSourceCancellation = false) {
    Finally finally([&trace] { trace.push_back(8); }, [&execution] {
        return execution.scope.abandoningFrames();
    });
    auto outcome = co_await execution.scope.host<Result<std::int64_t>>([&started] {
        started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return Result<std::int64_t>::Success(77);
    }, token);
    // Generated source evaluates statements after await before returning its
    // result. These budget steps must not impose a DI-only cancellation policy.
    execution.budget.step();
    if (explicitSourceCancellation && token.is_cancelled())
        co_return Result<std::int64_t>::Cancelled("fixture.explicit_source_cancelled");
    execution.budget.step();
    co_return outcome;
}
Coroutine<Result<std::int64_t>> ordinaryHostPolicy(ObjectOwner& objects, TaskScope& tasks,
    CancellationToken token, std::atomic<bool>& started, std::vector<int>& trace,
    bool explicitSourceCancellation) {
    GraphTransaction graph(objects.domain());
    EvaluationBudget budget;
    ObjectExecution execution(tasks, budget, graph, {});
    execution.cancellation = token;
    auto outcome = co_await waitHost(execution, token, started, trace, explicitSourceCancellation);
    graph.commit();
    co_return outcome;
}
void registerResource(const std::shared_ptr<ScopeExecution>& scope, std::vector<int>& trace, int marker) {
    const auto created = scope->construct<Counter>({}, [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
        auto reserved = attempt.reserve<Counter>(scope, "async.fixture", [&trace, marker](Counter ref,
            ObjectExecution& cleanup, CancellationToken token) {
            require(!token.is_cancelled(), "resource cleanup inherited cancelled operation token");
            require(cleanup.transaction.read<CounterState>(ref).value == marker, "cleanup graph was not alive");
            trace.push_back(marker);
            return Result<void>::Success();
        });
        const auto mark = attempt.allocationMark();
        auto ref = execution.transaction.create<CounterState>(CounterTag{});
        execution.transaction.mutate<CounterState>(ref, [marker](CounterState& state) { state.value = marker; });
        attempt.adopt(reserved, ref, mark);
        return ref;
    });
    require(created.is_success(), "async resource construction failed");
}
void isolatedGroups() {
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    auto first = owner->createScope(root).value();
    auto second = owner->createScope(root).value();
    std::vector<int> trace;
    registerResource(first, trace, 11);
    registerResource(second, trace, 22);
    bool firstReady = false;
    bool secondReady = false;
    auto firstTask = first->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
        return waitLocal(execution, token, firstReady, trace, 1);
    }).value();
    auto secondTask = second->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
        return waitLocal(execution, token, secondReady, trace, 2);
    }).value();
    require(!firstTask.completed() && !secondTask.completed() && root->pending().value() == 2,
            "independent pending groups were not admitted");
    auto closing = first->closeAsync().value();
    closing.cancel(); // Observing cancellation cannot undo irreversible Closing.
    require(!closing.completed() && first->checkOpen().is_failure() && second->checkOpen().is_success(),
            "Closing did not isolate one scope");
    require(root->poll().is_success(), "owner poll failed");
    require(firstTask.completed() && firstTask.result().is_cancelled() && closing.completed() &&
            closing.result().is_success(), "cancelled scope did not drain then close");
    require(trace == std::vector<int>({1, 11}), "cleanup ran before source task finally");
    require(!secondTask.completed() && second->pending().value() == 1,
            "one closing group cancelled an independent task");
    secondReady = true;
    require(root->poll().is_success(), "independent task poll failed");
    require(secondTask.result().is_success() && secondTask.result().value() == 2,
            "independent task lost authoritative success");
    require(root->close().is_success() && trace == std::vector<int>({1, 11, 2, 22}),
            "root did not dispose remaining scope after drain");
}
void hostOutcomeAndFinalRelease() {
    std::vector<int> trace;
    std::atomic<bool> started{false};
    std::weak_ptr<ExecutionOwner> lifetime;
    std::optional<Task<Result<std::int64_t>>> task;
    std::optional<Task<Result<void>>> closeTask;
    {
        auto owner = ExecutionOwner::Create().value();
        lifetime = owner;
        auto root = owner->rootExecution().value();
        registerResource(root, trace, 9);
        task = root->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
            return waitHost(execution, token, started, trace);
        }).value();
        for (int wait = 0; wait < 1000 && !started.load(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(started.load(), "host worker did not start");
        closeTask = root->closeAsync().value();
        require(!closeTask->completed(), "pending worker closed before delivery");
        // Final facade release blocks only for the bounded host operation, then
        // delivers its authoritative result/finally before resource cleanup.
    }
    require(lifetime.expired(), "task frame retained an owner cycle");
    require(task->completed() && task->result().is_success() && task->result().value() == 77,
            "teardown relabelled successful external host outcome");
    require(closeTask->completed() && closeTask->result().is_success() && trace == std::vector<int>({8, 9}),
            "final release did not drain then cleanup with observable close result");
}
void admissionAndTaskScopeContracts() {
    TaskScope first;
    TaskScope second;
    const auto group = first.createGroup();
    bool refused = false;
    try {
        (void)second.pending(group);
    } catch (const ExecutionFailure&) {
        refused = true;
    }
    require(refused, "foreign task group accepted");
    first.closeGroup(group);
    require(first.tryStart<Result<void>>(group, [](CancellationToken) -> Coroutine<Result<void>> {
        co_return Result<void>::Success();
    }).is_failure(), "closed group admitted work");
    first.shutdown();
    first.shutdown();
    require(first.pending() == 0, "shutdown was not idempotent");

    TaskOptions options;
    options.capacity = 1;
    auto owner = ExecutionOwner::Create(options).value();
    auto root = owner->rootExecution().value();
    bool ready = false;
    std::vector<int> trace;
    auto pending = root->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
        return waitLocal(execution, token, ready, trace, 3);
    }).value();
    const auto denied = root->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
        return waitLocal(execution, token, ready, trace, 4);
    });
    require(denied.is_failure() && trace.empty(), "capacity refusal started work or became a task result");
    auto close = root->closeAsync().value();
    require(root->poll().is_success() && pending.result().is_cancelled() && close.result().is_success(),
            "close supervision required an unavailable task slot");
}
void authoredCancellationAfterHostSuccessRemainsAuthoritative() {
    std::vector<int> trace;
    std::atomic<bool> started{false};
    auto owner = ExecutionOwner::Create().value();
    auto root = owner->rootExecution().value();
    registerResource(root, trace, 9);
    auto task = root->start<std::int64_t>({}, [&](ObjectExecution& execution, CancellationToken token) {
        return waitHost(execution, token, started, trace, true);
    }).value();
    for (int wait = 0; wait < 1000 && !started.load(); ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(started.load(), "authored-cancellation host worker did not start");
    auto closing = root->closeAsync().value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!closing.completed()) {
        if (!root->poll().is_success() || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("authored cancellation did not drain");
        std::this_thread::yield();
    }
    require(task.completed() && task.result().is_cancelled() &&
            task.result().error() == "fixture.explicit_source_cancelled",
            "runtime overrode explicit source cancellation after successful host work");
    require(closing.result().is_success() && trace == std::vector<int>({8, 9}),
            "authored cancellation failed to run finally before cleanup");
}
void ordinaryAndCompositionTaskPoliciesMatch() {
    for (const bool explicitSourceCancellation : {false, true}) {
        std::atomic<bool> started{false};
        std::vector<int> trace;
        ObjectOwner objects;
        TaskScope tasks;
        const auto task = tasks.start<Result<std::int64_t>>([&](CancellationToken token) {
            return ordinaryHostPolicy(objects, tasks, token, started, trace, explicitSourceCancellation);
        });
        for (int wait = 0; wait < 1000 && !started.load(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(started.load(), "ordinary-policy host worker did not start");
        tasks.shutdown();
        require(task.completed() && (explicitSourceCancellation
            ? task.result().is_cancelled() && task.result().error() == "fixture.explicit_source_cancelled"
            : task.result().is_success() && task.result().value() == 77),
            "ordinary and composition task policies disagree for the same coroutine body");
        require(trace == std::vector<int>({8}), "ordinary-policy source finally did not run");
    }
}
void retainedFinalDropDiagnostics() {
    std::optional<CleanupSession> rootSession;
    std::optional<CleanupSession> childSession;
    std::weak_ptr<ExecutionOwner> lifetime;
    {
        auto owner = ExecutionOwner::Create().value();
        lifetime = owner;
        auto root = owner->rootExecution().value();
        auto child = owner->createScope(root).value();
        rootSession = root->cleanupSession().value();
        childSession = child->cleanupSession().value();
        const auto add = [](const std::shared_ptr<ScopeExecution>& scope, bool cancellation) {
            return scope->construct<Counter>({}, [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
                const auto reserved = attempt.reserve<Counter>(scope, "retained.diagnostics",
                    [cancellation](Counter, ObjectExecution&, CancellationToken token) {
                        require(!token.is_cancelled(), "final-drop cleanup token was cancelled");
                        return cancellation ? Result<void>::Cancelled("fixture.unknown_cancelled")
                                            : Result<void>::Failure("fixture.unknown_failure");
                    });
                const auto mark = attempt.allocationMark();
                const auto ref = execution.transaction.create<CounterState>(CounterTag{});
                attempt.adopt(reserved, ref, mark);
                return ref;
            });
        };
        require(add(root, true).is_success() && add(child, false).is_success(), "diagnostic resources not admitted");
    }
    require(lifetime.expired(), "cleanup observer retained executor cycle");
    const auto rootStatus = rootSession->status().value();
    const auto childStatus = childSession->status().value();
    require(rootStatus.phase == ScopePhase::Closed && rootStatus.attempted == 2 && rootStatus.failed == 1 &&
            rootStatus.cancelled == 1 && rootStatus.unknown == 2 && rootStatus.pending == 0,
            "final root drop lost cleanup diagnostics");
    require(childStatus.phase == ScopePhase::Closed && childStatus.failed == 1 && childStatus.unknown == 1,
            "final child drop lost cleanup diagnostics");
    require(rootSession->task().completed() && rootSession->task().result().is_failure() &&
            childSession->task().completed() && childSession->task().result().is_failure(),
            "final-drop cleanup failures were silently successful");
    bool wrongOwnerRefused = false;
    std::thread observer([&] { wrongOwnerRefused = rootSession->status().is_failure(); });
    observer.join();
    require(wrongOwnerRefused, "cleanup observer ignored owner-thread contract");
}
}
int main() {
    try {
        isolatedGroups();
        hostOutcomeAndFinalRelease();
        admissionAndTaskScopeContracts();
        authoredCancellationAfterHostSuccessRemainsAuthoritative();
        ordinaryAndCompositionTaskPoliciesMatch();
        retainedFinalDropDiagnostics();
        std::cout << "PASS " << checks << " scoped async assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
