// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "AllocationProbe.hpp"
#include <cstdlib>
#include <new>
namespace {
thread_local bool recording = false;
thread_local di_tests::AllocationCounts counts;
void Record(std::size_t size) {
    if (recording) {
        ++counts.calls;
        counts.requestedBytes += size;
    }
}
void* Allocate(std::size_t size) {
    void* result = std::malloc(size == 0 ? 1 : size);
    if (!result)
        throw std::bad_alloc();
    Record(size);
    return result;
}
void* AllocateAligned(std::size_t size, std::size_t alignment) {
    void* result = nullptr;
    if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0)
        throw std::bad_alloc();
    Record(size);
    return result;
}
}
namespace di_tests {
void BeginAllocationProbe() { counts = {}; recording = true; }
AllocationCounts EndAllocationProbe() { recording = false; return counts; }
}
void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, static_cast<std::size_t>(alignment)); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
