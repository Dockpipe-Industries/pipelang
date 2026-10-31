// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Parallel.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using pipelang_models::ParallelModel;
using pipelang::model::List;

namespace {

List<std::int64_t> input() {
    auto values = List<std::int64_t>::Empty();
    for (std::int64_t item = 0; item < 8; ++item)
        values.add(item);
    return values;
}

template<class Task>
void finish(ParallelModel& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void semantics(std::size_t degree) {
    ParallelModel model({}, {degree});
    const auto values = input();
    assert(model.Sum(values).value() == 56);
    assert(model.LocalAtomic(values).value() == 8);
    assert(model.BorrowSnapshot(values, 3).value() == 108);
    assert(model.snapshot().marker == 3);
    assert(model.CompareBool().value());

    constexpr std::uint64_t negativeZero = 0x8000000000000000ULL;
    constexpr std::uint64_t quietNan = 0x7ff8000000000042ULL;
    assert(std::bit_cast<std::uint64_t>(
        model.CompareFloat(std::bit_cast<double>(negativeZero), 7.0).value()) == negativeZero);
    assert(model.ReadFloat().value() == 7.0);
    assert(std::bit_cast<std::uint64_t>(
        model.CompareFloat(std::bit_cast<double>(quietNan), 9.0).value()) == quietNan);
    assert(model.ReadFloat().value() == 9.0);

    const auto saved = model.snapshot();
    const auto failed = model.Fail(values);
    assert(failed.is_failure());
    assert(failed.error() == "lowest source fault");
    assert(model.snapshot() == saved);
    assert(model.ReadAtomic().value() == 8);
    assert(model.ReadCleanup().value() == 8);
    assert(model.LocalControl(values).value() == 9);
    assert(model.ReadCleanup().value() == 8);

    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    const auto beforeCancel = model.ReadAtomic().value();
    const auto cleanupBefore = model.ReadCleanup().value();
    const auto cancelled = model.RunCancelled(values, cancellation.token());
    assert(cancelled.is_cancelled());
    assert(model.ReadAtomic().value() == beforeCancel);
    assert(model.ReadCleanup().value() == cleanupBefore + 1);

    auto asyncCancelled = model.RunAsync(values, cancellation.token());
    finish(model, asyncCancelled);
    assert(asyncCancelled.result().is_cancelled());
    assert(model.ReadAtomic().value() == beforeCancel);
    assert(model.ReadCleanup().value() == cleanupBefore + 2);
    auto asyncSuccess = model.RunAsync(values, {});
    finish(model, asyncSuccess);
    assert(asyncSuccess.result().is_success());
    assert(asyncSuccess.result().value() == beforeCancel + 28);
    assert(model.ReadCleanup().value() == cleanupBefore + 3);
    assert(model.pending() == 0);
}

}

int main() {
    semantics(1);
    semantics(4);
}
