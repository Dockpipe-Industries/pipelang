// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Model.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace segment_test {
#ifdef PIPELANG_SEGMENTS_PROJECT
using App = execution_segments::source::Segments::App;
using SegmentHost = execution_segments::source::Segments::SegmentHost;
#else
using App = execution_segments::App;
using SegmentHost = execution_segments::SegmentHost;
#endif
using namespace pipelang::model;

inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class Host final : public SegmentHost {
public:
    Result<std::int64_t> Pause(CancellationToken token) override {
        std::unique_lock lock(mutex_);
        ++entered_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (held_ && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<std::int64_t>::Failure("segment fixture gate deadline");
            changed_.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (token.is_cancelled())
            return Result<std::int64_t>::Cancelled("segment fixture cancelled");
        if (fail_)
            return Result<std::int64_t>::Failure("segment fixture host failure");
        return Result<std::int64_t>::Success(1);
    }

    Result<void> Mark(std::int64_t value) override {
        std::lock_guard lock(mutex_);
        marks_.push_back(value);
        return Result<void>::Success();
    }

    void hold() {
        std::lock_guard lock(mutex_);
        held_ = true;
    }

    void release(bool fail = false) {
        std::lock_guard lock(mutex_);
        held_ = false;
        fail_ = fail;
        changed_.notify_all();
    }

    bool marked(std::int64_t value) const {
        std::lock_guard lock(mutex_);
        return std::find(marks_.begin(), marks_.end(), value) != marks_.end();
    }

    bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_ != 0;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool held_ = false;
    bool fail_ = false;
    std::size_t entered_ = 0;
    std::vector<std::int64_t> marks_;
};

template<class Pump, class Predicate>
void pumpUntil(Pump pump, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        pump();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("segment fixture owner deadline");
        std::this_thread::yield();
    }
}

template<class T>
void finish(App& app, const Task<T>& task) {
    pumpUntil([&] { app.poll(); }, [&] { return task.completed(); });
}

inline TaskOptions options(std::size_t steps) {
    TaskOptions selected;
    selected.evaluationStepsPerSegment = steps;
    return selected;
}

inline void expectPureExhaustion(App& app, std::int64_t iterations) {
    try {
        (void)app.Query(iterations);
    } catch (const EvaluationLimitFailure& error) {
        require(std::string(error.what()) == "source evaluation budget exceeded", "pure exhaustion changed identity");
        return;
    }
    throw std::runtime_error("pure work unexpectedly succeeded");
}
} // namespace segment_test
