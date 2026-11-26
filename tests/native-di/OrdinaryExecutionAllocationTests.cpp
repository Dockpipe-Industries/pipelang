// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/di/ScopeExecution.hpp>
#include "ExecutionCounterState.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

#if defined(__GNUC__) || defined(__clang__)
#define PIPELANG_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PIPELANG_TEST_NOINLINE __declspec(noinline)
#else
#define PIPELANG_TEST_NOINLINE
#endif
thread_local bool refuseAllocations = false;
PIPELANG_TEST_NOINLINE void* operator new(std::size_t size) {
    if (refuseAllocations)
        throw std::bad_alloc();
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
PIPELANG_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory) noexcept { ::operator delete(memory); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

int main() {
    using namespace pipelang::di;
    using namespace pipelang::model;
    using execution_test::CounterState;
    using execution_test::CounterTag;
    using Ref = ObjectRef<CounterTag>;
    try {
        auto owner = ExecutionOwner::Create().value();
        auto root = owner->rootExecution().value();
        const auto ref = root->construct<Ref>({}, [&](ObjectExecution& execution, ConstructionAttempt& attempt) {
            const auto reservation = attempt.reserve<Ref>(root, "allocation.fixture", [](Ref, ObjectExecution&, CancellationToken) {
                return Result<void>::Success();
            });
            const auto mark = attempt.allocationMark();
            auto value = execution.transaction.create<CounterState>(CounterTag{});
            attempt.adopt(reservation, value, mark);
            return value;
        }).value();
        const auto read = [&ref](ObjectExecution& execution) {
            return execution.transaction.read<CounterState>(ref).value;
        };
        (void)root->invoke<std::int64_t>({}, read).value(); // Pending publication sweep.
        (void)root->invoke<std::int64_t>({}, read).value();
        refuseAllocations = true;
        for (int call = 0; call < 1000; ++call) {
            if (!root->invoke<std::int64_t>({}, read).is_success())
                std::abort();
        }
        refuseAllocations = false;
        if (!root->close().is_success())
            throw std::runtime_error("resource close failed after no-allocation reads");
        std::cout << "PASS 1000 ordinary guarded calls without allocations with owned journal entries\n";
    } catch (const std::exception& error) {
        refuseAllocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
