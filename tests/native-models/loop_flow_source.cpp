// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
#ifdef PIPELANG_LOOP_FLOW_PROJECT
using App = loop_flow::source::LoopFlow::App;
using LoopObserver = loop_flow::source::LoopFlow::LoopObserver;
#else
using loop_flow::App;
using loop_flow::LoopObserver;
#endif
using namespace pipelang::model;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Operation>
bool exhaustsBudget(Operation operation) {
    try {
        operation();
    } catch (const EvaluationLimitFailure&) {
        return true;
    }
    return false;
}

class Observer final : public LoopObserver {
public:
    Result<void> Cleanup() override {
        ++cleanups;
        return Result<void>::Success();
    }

    Result<std::int64_t> Read(CancellationToken token) override {
        entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (hold && !released.load() && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<std::int64_t>::Failure("loop fixture read gate expired");
            std::this_thread::yield();
        }
        if (token.is_cancelled())
            return Result<std::int64_t>::Cancelled("loop fixture concrete read cancelled");
        ++reads;
        if (reads == 1)
            return Result<std::int64_t>::Success(0);
        return Result<std::int64_t>::Success(7);
    }

    std::atomic<bool> entered = false;
    std::atomic<bool> released = false;
    bool hold = false;
    int reads = 0;
    int cleanups = 0;
};

template<class TaskValue>
void finish(App& app, TaskValue& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        app.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("loop-flow fixture exceeded its outer watchdog");
        std::this_thread::yield();
    }
}

void cancellationAtAwait() {
    auto observer = std::make_shared<Observer>();
    observer->hold = true;
    App app(observer);
    CancellationSource cancellation;
    auto cancelled = app.AwaitedLoop(cancellation.token());
    const auto readinessDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!observer->entered.load()) {
        app.poll();
        if (std::chrono::steady_clock::now() >= readinessDeadline)
            throw std::runtime_error("loop cancellation fixture did not really suspend");
        std::this_thread::yield();
    }
    require(!cancelled.completed(), "loop cancellation fixture completed before its gate was released");
    cancellation.cancel();
    observer->released = true;
    finish(app, cancelled);
    require(cancelled.result().is_cancelled(), "awaited loop lost concrete-operation cancellation");
    require(observer->cleanups == 1, "cancellation did not execute source finally");
}
}

int main() {
    try {
        auto observer = std::make_shared<Observer>();
        App app(observer);
        require(exhaustsBudget([&] { (void)app.RawWhile(); }),
            "non-Result infinite while lost its evaluation bound");
        require(exhaustsBudget([&] { (void)app.RawFor(); }),
            "non-Result infinite for lost its evaluation bound");
        require(app.EmptyWhile().is_failure(), "empty literal-true while escaped evaluation bounds");
        require(observer->cleanups == 1, "while budget fault did not execute source finally");
        require(app.EmptyFor().is_failure(), "empty omitted-condition for escaped evaluation bounds");
        require(observer->cleanups == 2, "for budget fault did not execute source finally");
        const auto continued = app.WhileTransfers();
        require(continued.is_success() && continued.value() == 3, "while continue/break changed ordering");
        const auto iterated = app.ForTransfers();
        require(iterated.is_success() && iterated.value() == 3,
            "for continue must execute iteration and break must skip it");
        const auto propagated = app.PropagatingFor(false);
        require(propagated.is_success() && propagated.value() == 3,
            "literal-true propagation header lost continue/update or break ordering");
        const auto failedIteration = app.PropagatingFor(true);
        require(failedIteration.is_failure() && failedIteration.error() == "loop iteration failed",
            "literal-true propagation header lost iteration failure");
        auto observed = app.AwaitedLoop({});
        finish(app, observed);
        require(observed.result().is_success() && observed.result().value() == 7,
            "literal-true awaited loop lost repeated observation or return");
        require(observer->reads == 2 && observer->cleanups == 3,
            "awaited loop did not retain source sequencing and finally");
        cancellationAtAwait();
        require(app.WhileTransfers().is_success(), "terminal loop fault leaked the invocation budget");
        std::cout << "literal-true loop runtime contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
