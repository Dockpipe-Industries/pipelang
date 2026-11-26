// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeCellSlots.hpp"
#include "native_app_abi.hpp"

#include <pipelang/model/concurrent_collections.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeLists;
class HostedInput;
class HostedValue;

class NativeConcurrentSequences {
public:
    NativeConcurrentSequences();
    ~NativeConcurrentSequences();
    NativeConcurrentSequences(const NativeConcurrentSequences&) = delete;
    NativeConcurrentSequences& operator=(const NativeConcurrentSequences&) = delete;

    std::size_t checkpoint() const noexcept;
    void restore(std::size_t checkpoint) noexcept;
    void retire(std::uint64_t handle) noexcept;
    std::uint64_t create(ConcurrentSequenceKind kind, const OwnedShape& element);
    std::uint64_t borrow(const NativeConcurrentSequences& parent, std::uint64_t handle);
    std::uint64_t borrow(model::ConcurrentQueue<std::int64_t> queue);
    std::uint64_t borrow(const HostedValue& sequence);
    void call(std::uint64_t handle, ConcurrentSequenceOperation operation, const OwnedShape& element,
              const std::uint64_t* input, const OwnedShape* result, std::uint64_t* output,
              NativeLists& lists, HostedInput& packing) const;

private:
    struct Entry;
    const Entry& at(std::uint64_t handle) const;
    std::uint64_t retain(Entry entry);

    NativeCellSlots<Entry> cells_;
};

}
