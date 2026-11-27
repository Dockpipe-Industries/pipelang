// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/collection.hpp>
#include <pipelang/model/parallel.hpp>
#include <pipelang/model/task.hpp>

#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace pipelang::model;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const ExecutionFailure& error) {
        require(error.what() == message, "unexpected execution failure");
        return;
    }
    throw std::runtime_error("expected execution failure");
}

void consume(EvaluationBudget& budget, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        budget.step();
}

void localAndCleanup() {
    static_assert(defaultEvaluationSteps == 65536);
    static_assert(EvaluationBudget::maximumSteps == 65536);
    static_assert(TaskOptions{3, 77}.evaluationStepsPerSegment == 65536);
    static_assert(TaskOptions::maximumInitializationSteps == 1048576);
    static_assert(maximumEvaluationStepsPerSegment == std::numeric_limits<std::size_t>::max());
    failure([] { (void)EvaluationBudget::forInvocation(0); }, "evaluation steps per segment must be positive");
    failure([] { TaskScope scope(TaskOptions{4, 65536, 0}); }, "evaluation steps per segment must be positive");

    for (const std::size_t allowance : {1, 2, 70000}) {
        auto budget = EvaluationBudget::forInvocation(allowance);
        consume(budget, allowance);
        failure([&] { budget.step(); }, "source evaluation budget exceeded");
        {
            auto cleanup = budget.cleanup();
            consume(budget, defaultEvaluationSteps / 2);
            budget.reset();
            consume(budget, defaultEvaluationSteps / 2);
            failure([&] { budget.step(); }, "cleanup evaluation budget exceeded");
        }
        budget.reset();
        consume(budget, allowance);
        failure([&] { budget.step(); }, "source evaluation budget exceeded");
    }

    auto initialization = EvaluationBudget::forInitialization(2);
    consume(initialization, 2);
    initialization.reset();
    consume(initialization, defaultEvaluationSteps);
    failure([&] { initialization.step(); }, "source evaluation budget exceeded");
    auto largest = EvaluationBudget::forInvocation(maximumEvaluationStepsPerSegment);
    largest.step();
    largest.reset();
    largest.prepareParallel();
    auto child = largest.fork();
    child.step();

    TaskOptions selected;
    selected.evaluationStepsPerSegment = 70000;
    TaskScope scope(selected);
    selected.evaluationStepsPerSegment = 1;
    require(scope.evaluationStepsPerSegment() == 70000, "TaskScope did not retain its immutable selection");
    bool refused = false;
    std::thread other([&] {
        try {
            (void)scope.evaluationStepsPerSegment();
        } catch (const ExecutionFailure&) {
            refused = true;
        }
    });
    other.join();
    require(refused, "segment policy observation bypassed owner affinity");
}

void sharedAndCancellation() {
    auto budget = EvaluationBudget::forInvocation(5);
    budget.prepareParallel();
    auto first = budget.fork();
    auto second = budget.fork();
    consume(first, 2);
    consume(second, 3);
    failure([&] { budget.step(); }, "source evaluation budget exceeded");
    second.reset();
    consume(first, 5);
    failure([&] { second.step(); }, "source evaluation budget exceeded");
    {
        auto cleanup = first.cleanup();
        consume(first, defaultEvaluationSteps);
    }
    {
        auto cleanup = second.cleanup();
        failure([&] { second.step(); }, "cleanup evaluation budget exceeded");
    }

    auto items = List<std::int64_t>::Empty();
    for (int index = 0; index < 64; ++index)
        items.add(index);
    auto parallel = EvaluationBudget::forInvocation(4097);
    std::atomic<std::size_t> completed{0};
    failure([&] {
        parallelForEach(items, parallel, {}, {4}, [&](std::int64_t, EvaluationBudget& child) {
            for (int index = 0; index < 100; ++index) {
                child.step();
                completed.fetch_add(1);
            }
        });
    }, "source evaluation budget exceeded");
    require(completed.load() == 4097, "parallel workers minted independent allowances");

    auto cancelled = EvaluationBudget::forInvocation(1);
    cancelled.step();
    cancelled.setCancellationCheck([] { return false; });
    cancelled.setCancellationCheck([] { return true; });
    try {
        cancelled.step();
        throw std::runtime_error("cancelled exhausted budget returned success");
    } catch (const OperationCancelled&) {
    }
    auto cleanup = cancelled.cleanup();
    cancelled.step();
}
} // namespace

int main() {
    try {
        localAndCleanup();
        sharedAndCancellation();
        std::cout << "execution segment counters, shared work and cleanup contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
