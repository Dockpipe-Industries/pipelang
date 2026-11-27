// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/task.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <latch>
#include <stdexcept>
#include <vector>

using namespace pipelang::model;
using IntResult = Result<std::int64_t>;

namespace {

#ifdef __linux__
std::size_t threadCount() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++count;
    }
    return count;
}

void expectThreadCount(std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (threadCount() != expected) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
#endif

template<class Predicate>
void waitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

Coroutine<IntResult> synchronous() {
    co_return IntResult::Success(7);
}

Coroutine<IntResult> polling(TaskScope& scope, bool& ready, CancellationToken cancellation) {
    const bool result = co_await scope.pollUntil([&]() -> std::optional<bool> {
        if (ready)
            return true;
        return std::nullopt;
    }, cancellation);
    co_return IntResult::Success(result ? 1 : 0);
}

Coroutine<IntResult> hostJob(TaskScope& scope, std::function<IntResult()> function,
                             CancellationToken cancellation, int& resumed) {
    auto result = co_await scope.host<IntResult>(std::move(function), cancellation);
    ++resumed;
    co_return result;
}

void synchronousAndPollingScopes() {
#ifdef __linux__
    const auto baseline = threadCount();
#endif
    {
        // Generated synchronous models also own a TaskScope unconditionally.
        std::array<TaskScope, 16> scopes;
        for (auto& scope : scopes) {
            auto task = scope.start<IntResult>([](CancellationToken) {
                return synchronous();
            });
            assert(task.completed() && task.result().value() == 7);
            assert(scope.pending() == 0 && scope.poll() == 0);
        }
        bool ready = false;
        auto task = scopes[0].start<IntResult>([&](CancellationToken cancellation) {
            return polling(scopes[0], ready, cancellation);
        });
        assert(!task.completed());
        assert(scopes[0].poll() == 0);
#ifdef __linux__
        expectThreadCount(baseline);
#endif
        ready = true;
        assert(scopes[0].poll() == 1);
        assert(task.completed() && task.result().value() == 1);
    }
#ifdef __linux__
    expectThreadCount(baseline);
#endif
    std::optional<Task<IntResult>> retained;
    {
        TaskScope scope;
        bool ready = false;
        retained = scope.start<IntResult>([&](CancellationToken cancellation) {
            return polling(scope, ready, cancellation);
        });
    }
    assert(retained->completed() && retained->result().is_cancelled());
#ifdef __linux__
    expectThreadCount(baseline);
#endif
}

void firstSubmissionAndFifo() {
#ifdef __linux__
    const auto baseline = threadCount();
#endif
    {
        TaskScope scope(TaskOptions{4});
        std::latch entered(1);
        std::latch release(1);
        std::vector<int> executionOrder;
        std::vector<std::thread::id> workers;
        std::vector<Task<IntResult>> tasks;
        int resumed = 0;
        const auto owner = std::this_thread::get_id();
        for (int index = 0; index < 4; ++index) {
            tasks.push_back(scope.start<IntResult>([&, index](CancellationToken cancellation) {
                return hostJob(scope, [&, index] {
                    if (index == 0) {
                        entered.count_down();
                        release.wait();
                    }
                    workers.push_back(std::this_thread::get_id());
                    executionOrder.push_back(index);
                    return IntResult::Success(index);
                }, cancellation, resumed);
            }));
            if (index == 0) {
                entered.wait();
#ifdef __linux__
                expectThreadCount(baseline + 1);
#endif
            }
        }
        auto rejected = scope.start<IntResult>([](CancellationToken) {
            return synchronous();
        });
        assert(rejected.completed() && rejected.result().is_failure());
        assert(rejected.result().error() == "task capacity exceeded");
        assert(resumed == 0);
#ifdef __linux__
        expectThreadCount(baseline + 1);
#endif
        release.count_down();
        waitUntil([&] {
            scope.poll();
            return tasks.back().completed();
        });
        assert(resumed == 4);
        assert((executionOrder == std::vector<int>{0, 1, 2, 3}));
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            assert(tasks[index].result().value() == static_cast<std::int64_t>(index));
            assert(workers[index] == workers[0] && workers[index] != owner);
        }
        auto failed = scope.start<IntResult>([&](CancellationToken cancellation) {
            return hostJob(scope, []() -> IntResult {
                throw std::runtime_error("host fault");
            }, cancellation, resumed);
        });
        waitUntil([&] {
            scope.poll();
            return failed.completed();
        });
        assert(failed.result().is_failure() && failed.result().error() == "host fault");
#ifdef __linux__
        expectThreadCount(baseline + 1);
#endif
    }
#ifdef __linux__
    expectThreadCount(baseline);
#endif
}

void teardownDrainsAuthoritativeResults() {
    std::optional<Task<IntResult>> committed;
    std::optional<Task<IntResult>> queued;
    std::latch entered(1);
    int resumed = 0;
    bool queuedExecuted = false;
    {
        TaskScope scope;
        committed = scope.start<IntResult>([&](CancellationToken cancellation) {
            return hostJob(scope, [&, cancellation] {
                entered.count_down();
                // Destruction cancels entries before joining its worker. The
                // completed external outcome remains authoritative afterward.
                waitUntil([&] { return cancellation.is_cancelled(); });
                return IntResult::Success(42);
            }, cancellation, resumed);
        });
        entered.wait();
        queued = scope.start<IntResult>([&](CancellationToken cancellation) {
            return hostJob(scope, [&] {
                queuedExecuted = true;
                return IntResult::Success(9);
            }, cancellation, resumed);
        });
        assert(resumed == 0);
    }
    assert(committed->completed() && committed->result().value() == 42);
    assert(queued->completed() && queued->result().is_cancelled());
    assert(!queuedExecuted && resumed == 2);
}

}

int main() {
    synchronousAndPollingScopes();
    firstSubmissionAndFifo();
    teardownDrainsAuthoritativeResults();
}
