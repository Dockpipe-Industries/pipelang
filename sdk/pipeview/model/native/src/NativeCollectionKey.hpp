// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/result.hpp>

#include <cstring>
#include <limits>
#include <type_traits>

namespace pipelang::native_app_v19 {

enum class UnsignedCollectionKey : std::uint32_t {};

template<class Visitor>
decltype(auto) visitCollectionKey(const OwnedShape& shape, const char* kind, const Visitor& visit)
{
    switch (shape.kind) {
    case OwnedKind::Integer: return visit.template operator()<std::int64_t>();
    case OwnedKind::Boolean: return visit.template operator()<bool>();
    case OwnedKind::Text: return visit.template operator()<std::string>();
    case OwnedKind::UInt32:
    case OwnedKind::Enum: return visit.template operator()<UnsignedCollectionKey>();
    default: throw model::ExecutionFailure(std::string("invalid native ") + kind + " key kind");
    }
}

template<class Key>
Key loadCollectionKey(const OwnedShape& shape, const void* address, const char* kind)
{
    if constexpr (std::is_same_v<Key, std::string>) {
        TextView text;
        std::memcpy(&text, address, sizeof(text));
        if (text.size > model::maximumStringBytes || (text.size && !text.bytes))
            throw model::ExecutionFailure(std::string("invalid text key in native ") + kind);
        return text.size ? std::string(text.bytes, text.size) : std::string{};
    } else {
        std::uint64_t word;
        std::memcpy(&word, address, sizeof(word));
        if constexpr (std::is_same_v<Key, UnsignedCollectionKey>) {
            if (shape.kind == OwnedKind::Enum && word >= shape.valueCount)
                throw model::ExecutionFailure(std::string("invalid enum key in native ") + kind);
            if (word > std::numeric_limits<std::uint32_t>::max())
                throw model::ExecutionFailure(std::string("invalid uint32 key in native ") + kind);
        }
        if constexpr (std::is_same_v<Key, std::int64_t>) {
            std::int64_t integer;
            std::memcpy(&integer, &word, sizeof(integer));
            return integer;
        }
        return static_cast<Key>(word);
    }
}

template<class Key, class Visitor>
void visitCollectionKeyWords(const Key& value, const Visitor& visit)
{
    if constexpr (std::is_same_v<Key, std::string>) {
        const TextView text{value.data(), value.size()};
        visit(&text);
    } else {
        const auto bits = static_cast<std::uint64_t>(value);
        visit(&bits);
    }
}

}
