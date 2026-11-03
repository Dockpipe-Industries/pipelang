// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ListReadObjects.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <atomic>
#include <cstdlib>
#include <new>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <stdexcept>

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

namespace {
class ProjectionHost final : public pipelang_list_read_objects::ProjectionHost {
public:
    std::function<void()> callback;
    int calls = 0;
    pipelang::model::Result<void, std::string> Touch() override {
        ++calls;
        if (callback)
            callback();
        return pipelang::model::Result<void, std::string>::Success();
    }
};

template<class Callback>
std::size_t allocations(Callback callback) {
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations = true;
    try {
        callback();
        countAllocations = false;
        return allocationCount.load(std::memory_order_relaxed);
    } catch (...) {
        countAllocations = false;
        throw;
    }
}

void storedReads() {
    pipelang_list_read_objects::App model(std::make_shared<ProjectionHost>());
    const auto scalarOnce = allocations([&] { assert(model.ScalarReads(1) == 10); });
    const auto scalarMany = allocations([&] { assert(model.ScalarReads(128) == 1920); });
    const auto rowOnce = allocations([&] { assert(model.RecordReads(1) == 7); });
    const auto rowMany = allocations([&] { assert(model.RecordReads(128) == 1152); });
    const auto nestedOnce = allocations([&] { assert(model.NestedRecordReads(1) == 3); });
    const auto nestedMany = allocations([&] { assert(model.NestedRecordReads(128) == 512); });
    // Public query setup has fixed cost. Reading another element must not
    // decode/copy the complete stored sequence on each iteration.
    assert(scalarOnce == scalarMany);
    assert(rowOnce == rowMany);
    assert(nestedOnce == nestedMany);
    assert(model.NestedValues() == 17);
    for (int repetition = 0; repetition < 512; ++repetition)
        assert(model.CellValue(0) == 31 && model.CellValue(1) == 47);
    assert(model.RollbackSelected().is_failure());
    assert(model.CellValue(0) == 31);
    assert(model.RetainSelectedCell().value() == 48);

    const auto replaced = model.ReplaceIndex();
    assert(replaced.is_success() && replaced.value() == 20);
    assert(model.Size() == 1 && model.First() == 90 && model.Effects() == 1);
    assert(model.Reset().is_success());
    const auto appended = model.AppendIndex();
    assert(appended.is_failure() && appended.error() == "list index outside admitted range");
    assert(model.Size() == 2 && model.First() == 10 && model.Effects() == 0);
    for (const auto index : {-1, 2}) {
        bool failed = false;
        try {
            (void)pipelang_list_read_objects::App{std::make_shared<ProjectionHost>()}.Read(index);
        } catch (const pipelang::model::ExecutionFailure& error) {
            failed = true;
            assert(error.what() == std::string("list index outside admitted range"));
        }
        assert(failed);
    }
}
void recordProjections() {
    auto host = std::make_shared<ProjectionHost>();
    pipelang_list_read_objects::App model(host);
    const auto scalarOnce = allocations([&] { assert(model.ProjectScalars(1) == 7); });
    const auto scalarMany = allocations([&] { assert(model.ProjectScalars(128) == 896); });
    const auto nestedOnce = allocations([&] { assert(model.ProjectNested(1) == 11); });
    const auto nestedMany = allocations([&] { assert(model.ProjectNested(128) == 1408); });
    assert(scalarOnce == scalarMany);
    assert(nestedOnce == nestedMany);
    assert(model.ProjectListIsolation() == 1);
    for (int repetition = 0; repetition < 32; ++repetition)
        assert(model.TemporaryProjection().value() == 11);

    host->callback = [&] { assert(model.ProjectImageValue() == 11); };
    // Root reentry during an effect is refused; the source image remains intact.
    const auto reentered = model.ProjectAcrossHost();
    assert(reentered.is_failure());
    assert(reentered.error() == "model reentry during a synchronous host callback");
    assert(model.ProjectImageValue() == 11);
    host->callback = [] { throw std::runtime_error("projection host fault"); };
    const auto faulted = model.ProjectAcrossHost();
    assert(faulted.is_failure() && faulted.error() == "projection host fault");
    assert(model.ProjectImageValue() == 11);
    host->callback = {};
    const auto selected = model.ProjectAcrossHost();
    assert(selected.is_success() && selected.value() == 11);
    assert(model.ProjectImageValue() == 23);
    assert(host->calls == 3);

    std::atomic<bool> ownerRefused{false};
    std::thread other([&] {
        try {
            (void)model.ProjectImageValue();
        } catch (const pipelang::model::ExecutionFailure&) {
            ownerRefused = true;
        }
    });
    other.join();
    assert(ownerRefused);
}

}

int main() {
    storedReads();
    recordProjections();
    pipelang_list_read_objects::App model(std::make_shared<ProjectionHost>());
    for (int repetition = 0; repetition < 512; ++repetition) {
        assert(model.Read(0) == 6 && model.Read(1) == 9);
        assert(model.TemporaryResult() == 9);
    }
    assert(model.LocalReads(128) == 960);
}
