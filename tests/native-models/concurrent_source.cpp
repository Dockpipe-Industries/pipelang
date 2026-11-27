// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Concurrent.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <string>
using concurrent_models::ConcurrentModel;
using pipelang::model::List;

List<std::int64_t> input() {
    auto result = List<std::int64_t>::Empty();
    for (std::int64_t index = 0; index < 8; ++index)
        result.add(index);
    return result;
}

template<class Task>
void finish(ConcurrentModel& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void semantics(std::size_t degree) {
    ConcurrentModel model({}, {degree});
    assert(model.DefaultCounts().value() == 0);
    assert(model.CompoundOperations().value());
    assert(model.SequenceOperations().value());
    assert(model.GenericModuleHelper().value() == 13);
    assert(model.ParallelShared(input()).value() == 8028);
    assert(model.QueueCount().value() == 8);
    assert(model.StackCount().value() == 8);
    for (std::int64_t index = 0; index < 8; ++index)
        assert(model.MapValue(index).value() == index * 2);
    const auto ownedSnapshot = model.QueueSnapshot().value();
    assert(ownedSnapshot.count() == 8);
    assert(model.ParallelSnapshot().value() == 8006);
    assert(ownedSnapshot.count() == 8);
    assert(model.OrdinarySnapshot().value() == 403);
    auto task = model.AsyncLocalCapture();
    finish(model, task);
    assert(task.result().is_success() && task.result().value() == 2);
    assert(model.pending() == 0);
    pipelang::model::ConcurrentQueue<std::int64_t> external;
    auto externalTask = model.PublicAsyncHandle(external);
    finish(model, externalTask);
    assert(externalTask.result().is_success());
    assert(externalTask.result().value() == 1);
    assert(external.tryDequeue().value() == 8);
}

void rollbackAndCancellation() {
    ConcurrentModel model({}, {4});
    const auto before = model.snapshot();
    const auto failure = model.FailAfterSharedEffects();
    assert(failure.is_failure() && failure.error() == "after shared effects");
    assert(model.snapshot() == before);
    assert(model.MapValue(999).value() == 5);
    assert(model.QueueCount().value() == 1);
    assert(model.QueueSnapshot().value().at(0) == 99);

    pipelang::model::CancellationSource source;
    source.cancel();
    const auto cancelled = model.Cancelled(input(), source.token());
    assert(cancelled.is_cancelled());
    assert(model.snapshot() == before);
    assert(model.MapValue(777).value() == 7);
    assert(model.QueueCount().value() == 1);
    const auto capacity = model.ByteLimitPersistent(std::string(700000, 'a'), std::string(500000, 'b'));
    assert(capacity.is_failure());
    assert(model.snapshot() == before);
    assert(model.TextCount().value() == 1);
    assert(model.FailedTextEntryAbsent().value());
}

int main() {
    semantics(1);
    semantics(4);
    rollbackAndCancellation();
}
