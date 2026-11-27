// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
#ifdef PIPELANG_VAR_PROJECT
using App = var_inference::source::VarInference::App;
using Host = var_inference::source::VarInference::Host;
#else
using var_inference::App;
using var_inference::Host;
#endif
using namespace pipelang::model;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class Observer final : public Host {
public:
    Result<std::int64_t> Read(CancellationToken token) override {
        entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (hold && !released.load() && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<std::int64_t>::Failure("var.host_timeout");

            std::this_thread::yield();
        }
        if (token.is_cancelled())
            return Result<std::int64_t>::Cancelled("var.cancelled");

        if (fail)
            return Result<std::int64_t>::Failure("var.failed");

        return Result<std::int64_t>::Success(42);
    }

    Result<void> Cleanup() override {
        ++cleanups;
        return Result<void>::Success();
    }

    std::atomic<bool> entered = false;
    std::atomic<bool> released = false;
    bool hold = false;
    bool fail = false;
    int cleanups = 0;
};

template<class TaskValue>
void finish(App& app, TaskValue& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        app.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("inferred async fixture exceeded its outer watchdog");

        std::this_thread::yield();
    }
}

void suspendedCancellation() {
    auto observer = std::make_shared<Observer>();
    observer->hold = true;
    App app(observer);
    CancellationSource cancellation;
    auto task = app.Read(cancellation.token());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!observer->entered.load()) {
        app.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("inferred await did not enter the real host operation");

        std::this_thread::yield();
    }
    require(!task.completed(), "inferred await completed before the host gate was released");
    cancellation.cancel();
    observer->released = true;
    finish(app, task);
    require(task.result().is_cancelled(), "inferred await lost the concrete cancellation result");
    require(observer->cleanups == 1, "inferred cancellation cleanup did not run exactly once");
}
}

int main() {
    try {
        auto observer = std::make_shared<Observer>();
        App app(observer);
        require(app.Values(), "inferred scalar/generic/nullable values changed behavior");
        const ByteBuffer bytes(std::vector<std::uint8_t>{1, 2, 255});
        require(app.Collections(bytes) == 285, "inferred collection, byte or for-loop types changed behavior");
        List<std::int64_t> values;
        values.add(2);
        values.add(3);
        require(app.Parallel(values).value() == 5, "inferred parallel capture/item types changed behavior");

        auto success = app.Read({});
        finish(app, success);
        require(success.result().is_success() && success.result().value() == 42,
            "inferred local await lost its exact success type or value");
        require(observer->cleanups == 1, "successful inferred await lost its source cleanup");

        observer->fail = true;
        auto failure = app.Read({});
        finish(app, failure);
        require(failure.result().is_failure() && failure.result().error() == "var.failed",
            "inferred local await lost its exact failure payload");
        require(observer->cleanups == 2, "failed inferred await lost its source cleanup");
        suspendedCancellation();
        std::cout << "var inference runtime contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
