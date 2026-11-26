// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ListReads.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

namespace {
thread_local bool countAllocations = false;
std::atomic<std::size_t> allocationCount{0};
}

#if defined(_MSC_VER)
#define PIPELANG_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define PIPELANG_TEST_NOINLINE __attribute__((noinline))
#else
#define PIPELANG_TEST_NOINLINE
#endif

PIPELANG_TEST_NOINLINE void* operator new(std::size_t bytes) {
    if (countAllocations)
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes))
        return memory;
    throw std::bad_alloc();
}

PIPELANG_TEST_NOINLINE void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
PIPELANG_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#undef PIPELANG_TEST_NOINLINE

using namespace pipelang::model;
using namespace pipelang_list_reads;

namespace {
struct Measured {
    std::int64_t value;
    std::size_t allocations;
};

template<class Callback>
Measured measure(Callback callback) {
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations = true;
    try {
        const auto value = callback();
        countAllocations = false;
        return {value, allocationCount.load(std::memory_order_relaxed)};
    } catch (...) {
        countAllocations = false;
        throw;
    }
}

void repeatedOwnedReads() {
    ListReads model;
    const auto scalar = measure([&] { return model.ReadScalars(128); });
    const auto records = measure([&] { return model.ReadRows(128); });
    const auto counts = measure([&] { return model.CountRepeatedly(128); });
    assert(scalar.value == 1920 && scalar.allocations == 0);
    assert(records.value == 3072 && records.allocations == 0);
    assert(counts.value == 512 && counts.allocations == 0);
    assert(model.ConditionalIndex(false) == 10 && model.ConditionalIndex(true) == 20);

    // Value parameters and locals retain their fixed ownership copies; the
    // number of reads must not add an allocation per At operation.
    List<std::int64_t> input;
    input.add(3);
    input.add(5);
    const auto parameterOnce = measure([&] { return model.ReadParameter(input, 1); });
    const auto parameterMany = measure([&] { return model.ReadParameter(input, 128); });
    assert(parameterOnce.value == 3 && parameterMany.value == 512);
    assert(parameterOnce.allocations == parameterMany.allocations);
    const auto localOnce = measure([&] { return model.ReadLocal(1); });
    const auto localMany = measure([&] { return model.ReadLocal(128); });
    assert(localOnce.value == 10 && localMany.value == 1920);
    assert(localOnce.allocations == localMany.allocations);
}

void indexEffectsKeepTheReceiverSnapshot() {
    ListReads model;
    const auto replaced = model.ReplaceIndex();
    assert(replaced.is_success() && replaced.value() == 20);
    assert(model.Size() == 1 && model.First() == 90 && model.snapshot().effects == 1);
    assert(model.Reset().is_success());
    const auto failed = model.AppendIndex();
    assert(failed.is_failure() && failed.error() == "list index outside admitted range");
    assert(model.Size() == 2 && model.First() == 10 && model.snapshot().effects == 0);
}

void temporaryReceiversAndOwnedResults() {
    ListReads model;
    for (int repetition = 0; repetition < 512; ++repetition) {
        assert(model.TemporaryResult() == 20);
        assert(model.TemporaryList() == 10);
        assert(model.TemporaryCount() == 2);
    }
    auto detached = model.OwnedBag();
    detached.items.add(99);
    assert(detached.items.count() == 3);
    assert(model.OwnedBag().items.count() == 2);
}
}

int main() {
    repeatedOwnedReads();
    indexEffectsKeepTheReceiverSnapshot();
    temporaryReceiversAndOwnedResults();
}
