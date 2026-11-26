// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/collection.hpp>

#include <functional>
#include <memory>
#include <span>

namespace pipelang::native_app_v19 {

class NativeLists;
struct NativeDictionaryValue;

class NativeDictionaries {
public:
    NativeDictionaries();
    ~NativeDictionaries();

    struct Checkpoint {
        std::size_t versions;
        std::size_t bytes;
    };

    Checkpoint checkpoint() const noexcept;
    void restore(Checkpoint checkpoint) noexcept;

    void operate(NativeLists& lists, DictionaryOperation operation, const OwnedShape& shape,
                 DictionaryView& view, const void* key, const void* value, const OwnedShape* projection,
                 std::int64_t index, void* output);
    bool equal(DictionaryView left, DictionaryView right, const OwnedShape& shape) const;
    model::ListOwnership ownership(DictionaryView view, const OwnedShape& shape) const;
    void copyEntry(DictionaryView view, const OwnedShape& shape, std::int64_t index, void* output) const;
    DictionaryView import(NativeLists& lists, const OwnedShape& shape, std::size_t count,
                          const std::function<void(std::size_t, std::span<std::uint64_t>)>& read);

private:
    static NativeDictionaryValue capture(NativeLists& lists, const OwnedShape& shape, const void* value);
    class Versions;
    std::unique_ptr<Versions> versions_;
};

}
