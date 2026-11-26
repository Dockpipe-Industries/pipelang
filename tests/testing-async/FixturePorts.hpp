// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>

class FixturePorts final
    : public pipelang_testing_async::source::PipeLang::Testing::Async::TestClock,
      public pipelang_testing_async::source::PipeLang::Testing::AsyncSelfTests::AsyncFixture {
public:
    using VoidResult = pipelang::model::Result<void, std::string>;
    using IntResult = pipelang::model::Result<std::int64_t, std::string>;
    using BoolResult = pipelang::model::Result<bool, std::string>;

    IntResult NowMilliseconds() override {
        const auto remaining = successfulReadsBeforeFailure_.load();
        if (remaining == 0) {
            successfulReadsBeforeFailure_ = -1;
            return IntResult::Failure("fixture clock unavailable");
        }
        if (remaining > 0)
            successfulReadsBeforeFailure_ = remaining - 1;
        return IntResult::Success(milliseconds_);
    }

    VoidResult AdvanceMilliseconds(std::int64_t milliseconds) override {
        if (milliseconds < 0 || milliseconds_ > std::numeric_limits<std::int64_t>::max() - milliseconds)
            return VoidResult::Failure("invalid fixture clock advance");
        milliseconds_ += milliseconds;
        return VoidResult::Success();
    }

    VoidResult FailClockAfter(std::int64_t successfulReads) override {
        if (successfulReads < 0 || successfulReads > 128)
            return VoidResult::Failure("invalid fixture clock failure schedule");
        successfulReadsBeforeFailure_ = successfulReads;
        return VoidResult::Success();
    }

    VoidResult RequestBodyCancellation() override {
        bodyCancellation.cancel();
        return VoidResult::Success();
    }

    BoolResult Observe(std::string phase, std::int64_t remainingMilliseconds,
                       pipelang::model::CancellationToken token) override {
        if (phase.empty() || remainingMilliseconds <= 0)
            return BoolResult::Failure("observation requires a phase and remaining budget");
        const auto boundedMilliseconds = remainingMilliseconds < 3000 ? remainingMilliseconds : 3000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundedMilliseconds);
        while (!rootReleased.load()) {
            if (token.is_cancelled())
                return BoolResult::Cancelled("fixture observation cancelled");
            if (std::chrono::steady_clock::now() >= deadline)
                return BoolResult::Failure("fixture observation gate expired");
            std::this_thread::yield();
        }
        if (token.is_cancelled())
            return BoolResult::Cancelled("fixture observation cancelled");
        // This is the bounded native operation under test, not a test scheduler.
        // Source hooks choose all phases, cases, budgets and expected outcomes.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return BoolResult::Success(std::this_thread::get_id() != ownerThread_);
    }

    pipelang::model::CancellationSource bodyCancellation;
    std::atomic<bool> rootReleased{false};

private:
    const std::thread::id ownerThread_ = std::this_thread::get_id();
    std::int64_t milliseconds_ = 100;
    std::atomic<std::int64_t> successfulReadsBeforeFailure_{-1};
};
