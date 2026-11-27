// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
#ifdef PIPELANG_STATIC_PROJECT
using App = static_methods::source::StaticMethods::App;
#else
using static_methods::App;
#endif
using namespace pipelang::model;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class TaskValue>
void finish(App& app, TaskValue& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        app.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("static async fixture exceeded its outer watchdog");

        std::this_thread::yield();
    }
}
}

int main() {
    try {
        App app;
        require(app.Values(), "static generic/overload/local calls changed values or constructed a receiver");
        require(app.AssertionOutcomes(), "static assertions changed success, failure code or message");
        require(app.Compatibility().value(), "instance Assertions compatibility regressed");
        const auto written = app.Set(7, false);
        require(written.is_success() && written.value() == 7, "static effect lost its explicit receiver");
        const auto returnedFailure = app.Set(9, true);
        require(returnedFailure.is_failure() && returnedFailure.error() == "static.returned_failure" &&
            app.Current() == 9, "static effect changed normal-return Result publication semantics");
        const auto rolledBack = app.SetAndThrow(11);
        require(rolledBack.is_failure() && rolledBack.error() == "static.rollback" && app.Current() == 9,
            "escaping static fault did not roll back the caller transaction");
        require(app.Set(7, false).is_success(), "static rollback recovery failed");

        auto task = app.ReadAsync({});
        finish(app, task);
        require(task.result().is_success() && task.result().value() == 7,
            "static async call lost nested await results");

        require(app.Set(-3, false).is_success(), "static failure setup failed");
        auto failed = app.ReadAsync({});
        finish(app, failed);
        require(failed.result().is_failure() && failed.result().error() == "static.failed",
            "static async call lost the exact failure payload");
        require(app.Set(7, false).is_success(), "static recovery setup failed");

        CancellationSource cancellation;
        cancellation.cancel();
        auto cancelled = app.ReadAsync(cancellation.token());
        finish(app, cancelled);
        require(cancelled.result().is_cancelled(), "static async call lost explicit cancellation");

        bool bounded = false;
        try {
            (void)app.ExhaustDepth();
        } catch (const ExecutionFailure&) {
            bounded = true;
        }
        require(bounded, "static recursion bypassed source call depth limits");
        require(app.Values(), "static recursion leaked invocation state");
        std::cout << "static source runtime contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
