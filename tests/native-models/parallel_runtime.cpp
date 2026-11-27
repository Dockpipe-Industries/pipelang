// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/atomic.hpp>
#include <pipelang/model/parallel.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <atomic>
#include <barrier>
#include <bit>
#include <cassert>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace pipelang::model;

namespace {

template<class Function>
void expectFailure(Function function) {
    bool failed = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

List<std::int64_t> items(std::int64_t count) {
    auto values = List<std::int64_t>::Empty();
    for (std::int64_t index = 0; index < count; ++index)
        values.add(index);
    return values;
}

class ActiveCall {
public:
    explicit ActiveCall(std::atomic<int>& active) : active_(active) {
        active_.fetch_add(1);
    }
    ~ActiveCall() { active_.fetch_sub(1); }
private:
    std::atomic<int>& active_;
};

void atomicValues() {
    Atomic<std::int64_t> integer;
    assert(integer.load() == 0);
    auto shared = integer;
    shared.store(4);
    assert(integer.exchange(8) == 4);
    assert(integer.compareExchange(10, 9) == 8);
    assert(integer.load() == 8);
    assert(integer.compareExchange(10, 8) == 8);
    assert(integer.add(5) == 15);
    assert(integer.increment() == 16);
    assert(integer.decrement() == 15);

    integer.store(std::numeric_limits<std::int64_t>::max());
    expectFailure([&] { integer.increment(); });
    assert(integer.load() == std::numeric_limits<std::int64_t>::max());
    integer.store(std::numeric_limits<std::int64_t>::min());
    expectFailure([&] { integer.decrement(); });
    assert(integer.load() == std::numeric_limits<std::int64_t>::min());

    auto moved = std::move(shared);
    validate(moved);
    expectFailure([&] { validate(shared); });
    expectFailure([&] { shared.load(); });

    Atomic<bool> flag;
    assert(!flag.load());
    assert(!flag.compareExchange(true, false));
    assert(flag.exchange(false));

    constexpr std::uint64_t negativeZeroBits = 0x8000000000000000ULL;
    constexpr std::uint64_t quietNanBits = 0x7ff8000000000042ULL;
    constexpr std::uint64_t otherNanBits = 0x7ff8000000000043ULL;
    auto floating = Atomic<double>::Create(std::bit_cast<double>(negativeZeroBits));
    auto observed = floating.compareExchange(1.0, 0.0);
    assert(std::bit_cast<std::uint64_t>(observed) == negativeZeroBits);
    assert(std::bit_cast<std::uint64_t>(floating.load()) == negativeZeroBits);
    floating.store(std::bit_cast<double>(quietNanBits));
    observed = floating.compareExchange(2.0, std::bit_cast<double>(otherNanBits));
    assert(std::bit_cast<std::uint64_t>(observed) == quietNanBits);
    assert(std::bit_cast<std::uint64_t>(floating.load()) == quietNanBits);
    observed = floating.compareExchange(3.0, std::bit_cast<double>(quietNanBits));
    assert(std::bit_cast<std::uint64_t>(observed) == quietNanBits);
    assert(floating.load() == 3.0);
}

void boundedOverlap(std::size_t degree) {
    EvaluationBudget budget;
    Atomic<std::int64_t> total;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::barrier together(static_cast<std::ptrdiff_t>(degree));
    parallelForEach(items(static_cast<std::int64_t>(degree)), budget, {}, {degree},
                    [&](std::int64_t, EvaluationBudget& child) {
        ActiveCall call(active);
        int observed = peak.load();
        const auto current = active.load();
        while (observed < current && !peak.compare_exchange_weak(observed, current)) {}
        together.arrive_and_wait();
        for (int index = 0; index < 1000; ++index) {
            child.step();
            total.increment();
        }
    });
    assert(active.load() == 0);
    assert(peak.load() == static_cast<int>(degree));
    assert(total.load() == static_cast<std::int64_t>(degree * 1000));
}

void deterministicFailureAndJoin() {
    EvaluationBudget budget;
    std::atomic<int> active{0};
    Atomic<std::int64_t> effects;
    std::barrier together(4);
    std::latch higherFaultStarted(1);
    bool failed = false;
    try {
        parallelForEach(items(4), budget, {}, {4},
                        [&](std::int64_t index, EvaluationBudget&) {
            ActiveCall call(active);
            effects.increment();
            together.arrive_and_wait();
            if (index == 2) {
                higherFaultStarted.count_down();
                throw ExecutionFailure("higher input index");
            }
            if (index == 0) {
                higherFaultStarted.wait();
                throw ExecutionFailure("lowest input index");
            }
        });
    } catch (const ExecutionFailure& error) {
        failed = true;
        assert(std::string(error.what()) == "lowest input index");
    }
    assert(failed);
    assert(active.load() == 0);
    assert(effects.load() == 4);
}

void cancellationAndFailurePrecedence() {
    for (bool fail : {false, true}) {
        EvaluationBudget budget;
        CancellationSource source;
        std::atomic<int> active{0};
        Atomic<std::int64_t> effects;
        std::barrier together(4);
        std::latch cancellationRequested(1);
        bool completed = false;
        try {
            parallelForEach(items(4), budget, source.token(), {4},
                            [&](std::int64_t index, EvaluationBudget& child) {
                ActiveCall call(active);
                effects.increment();
                together.arrive_and_wait();
                if (index == 0) {
                    source.cancel();
                    cancellationRequested.count_down();
                    if (fail)
                        throw ExecutionFailure("fault wins cancellation");
                }
                cancellationRequested.wait();
                child.step();
            });
        } catch (const OperationCancelled&) {
            assert(!fail);
            completed = true;
        } catch (const ExecutionFailure& error) {
            assert(fail);
            assert(std::string(error.what()) == "fault wins cancellation");
            completed = true;
        }
        assert(completed);
        assert(active.load() == 0);
        assert(effects.load() == 4);
    }
}

void sharedBudgetAndAdmission() {
    EvaluationBudget budget;
    Atomic<std::int64_t> completedSteps;
    expectFailure([&] {
        parallelForEach(items(8), budget, {}, {4},
                        [&](std::int64_t, EvaluationBudget& child) {
            for (std::size_t step = 0; step < EvaluationBudget::maximumSteps; ++step) {
                child.step();
                completedSteps.increment();
            }
        });
    });
    assert(completedSteps.load() == static_cast<std::int64_t>(EvaluationBudget::maximumSteps));

    EvaluationBudget invalidBudget;
    expectFailure([&] {
        parallelForEach(items(1), invalidBudget, {}, {0},
                        [](std::int64_t, EvaluationBudget&) {});
    });
    expectFailure([&] {
        parallelForEach(items(1), invalidBudget, {}, {33},
                        [](std::int64_t, EvaluationBudget&) {});
    });
    CancellationSource cancelled;
    cancelled.cancel();
    bool sawCancellation = false;
    try {
        parallelForEach(items(1), invalidBudget, cancelled.token(), {},
                        [](std::int64_t, EvaluationBudget&) { assert(false); });
    } catch (const OperationCancelled&) {
        sawCancellation = true;
    }
    assert(sawCancellation);
}

struct SubmissionState {
    std::atomic<int> copies{0};
    std::atomic<int> active{0};
    std::latch firstWorkerEntered{1};
    std::latch releaseWorker{1};
};

class ThrowingCallbackCopy {
public:
    explicit ThrowingCallbackCopy(std::shared_ptr<SubmissionState> state)
        : state_(std::move(state)) {}
    ThrowingCallbackCopy(const ThrowingCallbackCopy& other) : state_(other.state_) {
        if (state_->copies.fetch_add(1) == 1) {
            state_->firstWorkerEntered.wait();
            state_->releaseWorker.count_down();
            throw ExecutionFailure("callback submission copy failed");
        }
    }
    ThrowingCallbackCopy(ThrowingCallbackCopy&&) noexcept = default;
    void operator()(std::int64_t, EvaluationBudget&) const {
        ActiveCall call(state_->active);
        state_->firstWorkerEntered.count_down();
        state_->releaseWorker.wait();
    }
private:
    std::shared_ptr<SubmissionState> state_;
};

void submissionFailureJoins() {
    EvaluationBudget budget;
    auto state = std::make_shared<SubmissionState>();
    expectFailure([&] {
        parallelForEach(items(8), budget, {}, {4}, ThrowingCallbackCopy(state));
    });
    assert(state->copies.load() == 2);
    assert(state->active.load() == 0);
}

}

int main() {
    atomicValues();
    boundedOverlap(1);
    boundedOverlap(4);
    deterministicFailureAndJoin();
    cancellationAndFailurePrecedence();
    sharedBudgetAndAdmission();
    submissionFailureJoins();
}
