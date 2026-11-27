// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/task.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace pipelang::model;
using IntResult = Result<std::int64_t>;

namespace {
using Guard = TaskScope::SynchronousHostCallGuard;
static_assert(!std::is_copy_constructible_v<Guard>);
static_assert(!std::is_copy_assignable_v<Guard>);
static_assert(std::is_nothrow_move_constructible_v<Guard>);
static_assert(std::is_nothrow_move_assignable_v<Guard>);

template<class Callback>
void expectFailure(Callback callback, const char* message) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure& error) {
        failed = true;
        assert(error.what() == std::string(message));
    }
    assert(failed);
}

void expectReentryRefused(TaskScope& scope) {
    constexpr auto message = "model reentry during a synchronous host callback";
    // Generated public synchronous actions and queries call requireOwner.
    expectFailure([&] { scope.requireOwner(); }, message);
    expectFailure([&] { (void)scope.pending(); }, message);
    expectFailure([&] { (void)scope.poll(); }, message);
    expectFailure([&] { (void)scope.pollUntil([] { return std::optional<bool>(true); }); }, message);
    expectFailure([&] { auto nested = scope.enterSynchronousHostCall(); }, message);
}

Coroutine<IntResult> immediate() {
    co_return IntResult::Success(7);
}

void ownerReentryAndIndependentScope() {
    TaskScope scope;
    TaskScope other;
    bool factoryEntered = false;
    {
        auto guard = scope.enterSynchronousHostCall();
        expectReentryRefused(scope);
        expectFailure([&] {
            scope.start<IntResult>([&](CancellationToken) {
                factoryEntered = true;
                return immediate();
            });
        }, "model reentry during a synchronous host callback");
        assert(!factoryEntered);
        other.requireOwner();
        assert(other.poll() == 0 && other.pending() == 0);
        {
            auto otherGuard = other.enterSynchronousHostCall();
            expectReentryRefused(other);
            expectReentryRefused(scope);
        }
        other.requireOwner();
        expectReentryRefused(scope);
    }
    scope.requireOwner();
    assert(scope.poll() == 0 && scope.pending() == 0);
}

void movesAndExceptionRestoreExactlyOnce() {
    TaskScope first;
    TaskScope second;
    {
        std::optional<Guard> original(first.enterSynchronousHostCall());
        std::optional<Guard> moved(std::move(*original));
        original.reset();
        expectReentryRefused(first);
        moved.reset();
        first.requireOwner();
    }
    {
        auto firstGuard = first.enterSynchronousHostCall();
        auto secondGuard = second.enterSynchronousHostCall();
        firstGuard = std::move(secondGuard);
        first.requireOwner();
        expectReentryRefused(second);
        {
            auto moved = std::move(firstGuard);
            expectReentryRefused(second);
        }
        second.requireOwner();
    }
    try {
        auto guard = first.enterSynchronousHostCall();
        throw std::runtime_error("callback fault");
    } catch (const std::runtime_error& error) {
        assert(error.what() == std::string("callback fault"));
    }
    first.requireOwner();
    auto guard = first.enterSynchronousHostCall();
    expectReentryRefused(first);
}

void wrongThreadCannotEnterOrClearGuard() {
    TaskScope scope;
    const auto rejected = [&] {
        expectFailure([&] { scope.requireOwner(); }, "model accessed from a different owner thread");
        expectFailure([&] { auto guard = scope.enterSynchronousHostCall(); },
            "model accessed from a different owner thread");
    };
    std::thread outside(rejected);
    outside.join();
    {
        auto guard = scope.enterSynchronousHostCall();
        std::thread during(rejected);
        during.join();
        expectReentryRefused(scope);
    }
    scope.requireOwner();
}

Coroutine<IntResult> hostJob(TaskScope& scope, std::thread::id& worker,
                             CancellationToken cancellation) {
    auto result = co_await scope.host<IntResult>([&] {
        worker = std::this_thread::get_id();
        return IntResult::Success(42);
    }, cancellation);
    co_return result;
}

void asyncWorkerAndOwnerPumpRemainAvailable() {
    TaskScope scope;
    std::thread::id worker;
    const auto owner = std::this_thread::get_id();
    auto task = scope.start<IntResult>([&](CancellationToken cancellation) {
        return hostJob(scope, worker, cancellation);
    });
    {
        auto guard = scope.enterSynchronousHostCall();
        expectReentryRefused(scope);
        // A host completion must not run a model continuation reentrantly.
        assert(!task.completed());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        assert(std::chrono::steady_clock::now() < deadline);
        scope.poll();
        std::this_thread::yield();
    }
    assert(task.result().is_success() && task.result().value() == 42);
    assert(worker != owner && worker != std::thread::id{});
    assert(scope.pending() == 0);
}
}

int main() {
    ownerReentryAndIndependentScope();
    movesAndExceptionRestoreExactlyOnce();
    wrongThreadCannotEnterOrClearGuard();
    asyncWorkerAndOwnerPumpRemainAvailable();
}
