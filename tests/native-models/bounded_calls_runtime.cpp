// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/cleanup.hpp>
#include <pipelang/model/object_model.hpp>
#include <pipelang/model/parallel.hpp>
#include <pipelang/model/source_call.hpp>

#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace pipelang::model;
using IntResult = Result<std::int64_t>;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void limitFailure(Function function) {
    try {
        function();
    } catch (const EvaluationLimitFailure&) {
        ++assertions;
        return;
    }
    throw std::runtime_error("missing terminal limit failure");
}

void ancestryAndWork() {
    const SourceCallContext root;
    auto deepest = root;
    for (std::size_t depth = 1; depth <= SourceCallContext::maximumDepth; ++depth) {
        deepest = deepest.enter();
        require(deepest.depth() == depth, "source depth count changed");
    }
    limitFailure([&] { (void)deepest.enter(); });
    require(root.depth() == 0 && root.enter().depth() == 1, "child modified sibling ancestry");

    EvaluationBudget budget;
    for (std::size_t index = 0; index < defaultEvaluationSteps; ++index)
        budget.step();
    limitFailure([&] { budget.step(); });
    {
        auto cleanup = budget.cleanup();
        for (std::size_t index = 0; index < defaultEvaluationSteps; ++index)
            budget.step();
        limitFailure([&] { budget.step(); });
        limitFailure([&] { (void)deepest.enter(); });
    }
    budget.reset();
    budget.step();
    limitFailure([&] { (void)deepest.enter(); });
}

void executionLifetime() {
    TaskScope scope;
    EvaluationBudget budget;
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    int commits = 0;
    auto callbacks = std::make_shared<ObjectRootCallbacks>();
    callbacks->context = &commits;
    callbacks->commit = [](void* context) { ++*static_cast<int*>(context); };
    std::weak_ptr<ObjectRootCallbacks> weak = callbacks;
    std::optional<ObjectExecution> retained;
    CancellationSource cancellation;
    {
        ObjectExecution root(scope, budget, transaction, callbacks);
        root.cancellation = cancellation.token();
        const auto owners = callbacks.use_count();
        auto first = root.enterCall();
        auto second = first.enterCall();
        require(callbacks.use_count() == owners, "synchronous calls copied the callback lease");
        require(first.calls().depth() == 1 && second.calls().depth() == 2, "execution ancestry was lost");
        second.commitSegment();
        retained.emplace(second.enterAsyncCall());
        require(callbacks.use_count() == owners + 1, "async frame did not retain its original callback lease");
    }
    callbacks.reset();
    require(!weak.expired(), "async execution retained a borrowed stack lease");
    cancellation.cancel();
    require(retained->cancellationToken().is_cancelled(), "async cancellation lease was not retained");
    retained->commitSegment();
    require(commits == 2, "borrowed or retained callback target changed");
    retained.reset();
    require(weak.expired(), "async execution leaked its callback lease");
}

Coroutine<IntResult> exhaustedChild() {
    throw EvaluationLimitFailure("terminal child work");
    co_return IntResult::Success(1);
}

Coroutine<IntResult> ordinaryChild() {
    co_return IntResult::Failure("ordinary child failure");
}

Coroutine<IntResult> parent(bool terminal, int& cleaned) {
    Finally cleanup([&] { ++cleaned; }, [] { return false; });
    auto child = terminal ? exhaustedChild() : ordinaryChild();
    const auto result = co_await std::move(child);
    if (result.is_failure())
        co_return IntResult::Success(19);
    co_return result;
}

void terminalPropagation() {
    TaskScope scope;
    int cleaned = 0;
    auto terminal = scope.start<IntResult>([&](CancellationToken) { return parent(true, cleaned); });
    require(terminal.completed() && terminal.result().is_failure() &&
        terminal.result().error() == "terminal child work", "await lost terminal exception identity");
    require(cleaned == 1, "terminal await skipped or repeated parent cleanup");
    auto ordinary = scope.start<IntResult>([&](CancellationToken) { return parent(false, cleaned); });
    require(ordinary.completed() && ordinary.result().value() == 19,
        "ordinary child Failure stopped being a recoverable value");
    require(cleaned == 2 && scope.pending() == 0, "coroutine result or frame ownership leaked");
}

void parallelInheritance() {
    EvaluationBudget budget;
    auto parent = SourceCallContext{};
    for (std::size_t depth = 0; depth < 31; ++depth)
        parent = parent.enter();
    auto items = List<std::int64_t>::Empty();
    items.add(0);
    items.add(1);
    std::atomic<unsigned> completed{0};
    parallelForEach(items, budget, {}, {2}, [parent, &completed](std::int64_t, EvaluationBudget& childBudget) {
        const auto child = parent.enter();
        if (child.depth() != 32)
            throw std::runtime_error("worker lost inherited source depth");
        // Each worker consumes precisely half of one shared work allowance.
        for (std::size_t index = 0; index < defaultEvaluationSteps / 2; ++index)
            childBudget.step();
        ++completed;
    });
    require(completed == 2 && parent.depth() == 31, "parallel sibling ancestry interfered");
    limitFailure([&] { budget.step(); });
    budget.reset();
    limitFailure([&] {
        parallelForEach(items, budget, {}, {2}, [parent](std::int64_t, EvaluationBudget&) {
            (void)parent.enter().enter();
        });
    });
}

}

int main() {
    try {
        ancestryAndWork();
        executionLifetime();
        terminalPropagation();
        parallelInheritance();
        std::cout << "bounded call runtime: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
