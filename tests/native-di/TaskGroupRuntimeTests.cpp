// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/task.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pipelang::model;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Exercises an exceptional native promise path. Ordinary source Result never
// intentionally throws here, but a failed error allocation can also escape resume.
struct ThrowingOutcome {
    static ThrowingOutcome Cancelled(const std::string&) { return {}; }
    static ThrowingOutcome Failure(const std::string&) { throw std::runtime_error("fixture.promise_failure"); }
};
Coroutine<ThrowingOutcome> throwsDuringResume() {
    throw std::runtime_error("fixture.body_failure");
    co_return ThrowingOutcome{};
}
Coroutine<Result<void>> wait(TaskScope& scope, CancellationToken token, bool& ready) {
    (void)co_await scope.pollUntil([&ready]() -> std::optional<bool> {
        if (ready)
            return true;
        return std::nullopt;
    }, token);
    co_return Result<void>::Success();
}
Coroutine<Result<void>> shutdownReentry(TaskScope& scope, const std::shared_ptr<TaskGroup>& group) {
    bool shutdownRefused = false;
    bool drainRefused = false;
    try {
        scope.shutdown();
    } catch (const ExecutionFailure& error) {
        shutdownRefused = std::string(error.what()) == "task shutdown during an active continuation";
    }
    try {
        scope.drainGroup(group);
    } catch (const ExecutionFailure& error) {
        drainRefused = std::string(error.what()) == "task drain during an active continuation";
    }
    require(shutdownRefused && drainRefused, "active continuation reentered task drain/shutdown");
    co_return Result<void>::Success();
}
}
int main() {
    try {
        TaskScope scope;
        const auto group = scope.createGroup();
        const auto reentry = scope.tryStart<Result<void>>(group, [&](CancellationToken) {
            return shutdownReentry(scope, group);
        }).value();
        require(reentry.completed() && reentry.result().is_success(), "rejected task drain corrupted active operation");
        bool failed = false;
        try {
            (void)scope.tryStart<ThrowingOutcome>(group, [](CancellationToken) { return throwsDuringResume(); });
        } catch (const std::runtime_error& error) {
            failed = std::string(error.what()) == "fixture.promise_failure";
        }
        require(failed, "exceptional resume witness did not execute");
        bool ready = false;
        auto independent = scope.start<Result<void>>([&](CancellationToken token) { return wait(scope, token, ready); });
        scope.closeGroup(group);
        (void)scope.poll();
        require(!independent.completed(), "exceptional resume leaked active task group into later work");
        ready = true;
        (void)scope.poll();
        require(independent.completed() && independent.result().is_success(), "restored independent group did not complete");
        scope.shutdown();
        std::cout << "PASS task group restoration after exceptional coroutine resume\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
