// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeCellSlots.hpp"
#include "native_app_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeLists;
class HostedInput;
class HostedValue;

class NativeConcurrentDictionaries {
public:
    NativeConcurrentDictionaries();
    ~NativeConcurrentDictionaries();
    NativeConcurrentDictionaries(const NativeConcurrentDictionaries&) = delete;
    NativeConcurrentDictionaries& operator=(const NativeConcurrentDictionaries&) = delete;

    std::size_t checkpoint() const noexcept;
    void restore(std::size_t checkpoint) noexcept;
    void retire(std::uint64_t handle) noexcept;
    std::uint64_t create(const OwnedShape& key, const OwnedShape& value);
    std::uint64_t borrow(const NativeConcurrentDictionaries& parent, std::uint64_t handle);
    std::uint64_t borrow(const HostedValue& dictionary);
    void call(std::uint64_t handle, ConcurrentDictionaryOperation operation,
              const OwnedShape& keyShape, const OwnedShape& valueShape, const std::uint64_t* key,
              const std::uint64_t* input, const std::uint64_t* comparison,
              const OwnedShape* result, std::uint64_t* output, NativeLists& lists, HostedInput& packing) const;

private:
    struct Entry;
    const Entry& at(std::uint64_t handle) const;
    std::uint64_t retain(Entry entry);

    NativeCellSlots<Entry> cells_;
};

}
