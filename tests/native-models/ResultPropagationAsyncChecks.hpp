// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#ifndef __EMSCRIPTEN__
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace propagation_checks {
class Gate final : public result_propagation::PropagationGate {
public:
    Result<std::int64_t> Read(std::int64_t mode, pipelang::model::CancellationToken cancellation) override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        if (!condition_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; }))
            return Result<std::int64_t>::Failure("test gate timed out");
        if (cancellation.is_cancelled() || mode == 2)
            return Result<std::int64_t>::Cancelled("gate cancelled");
        if (mode == 1)
            return Result<std::int64_t>::Failure("gate refused");
        return Result<std::int64_t>::Success(7);
    }

    void releaseAfterEntry() {
        std::unique_lock lock(mutex_);
        const bool entered = condition_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_; });
        assert(entered);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

inline void verifySuspended() {
    for (std::int64_t mode = 0; mode < 3; ++mode) {
        const auto host = std::make_shared<Gate>();
        result_propagation::SuspendedPropagation model(host);
        const auto task = model.Assign(mode, {});
        assert(!task.completed());
        assert(model.snapshot().value == 3 && model.snapshot().cleanup == 0);
        host->releaseAfterEntry();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed() && std::chrono::steady_clock::now() < deadline) {
            model.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(task.completed());
        const auto outcome = task.result();
        if (mode == 0)
            assert(outcome.is_success() && outcome.value() == 7);
        else if (mode == 1)
            assert(outcome.is_failure() && outcome.error() == "gate refused");
        else
            assert(outcome.is_cancelled() && outcome.error() == "gate cancelled");
        const auto snapshot = model.snapshot();
        assert(snapshot.value == (mode == 0 ? 7 : 3));
        assert(snapshot.cleanup == 1);
    }
}
}
#endif
