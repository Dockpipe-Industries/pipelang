// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace pipelang::models::native_ir {

enum class ScalarType {
    Boolean, Integer, Enum, Receiver, List, Array, Text, Error, Result, Record, Optional,
    Semaphore, CancellationToken, Service, Interface, HashSet, UInt32, Float, Float32, ByteBuffer, Queue, Stack,
    Dictionary, Pair, ClassReference, FixedArray, AtomicReference, LockReference, ConcurrentSequenceReference, ConcurrentDictionaryReference,
    ResultInterface, ContextOwnerReference
};

struct NativeType {
    constexpr NativeType(ScalarType kind, std::size_t layout = 0) : kind(kind), layout(layout) {}
    bool operator==(const NativeType&) const = default;
    bool operator==(ScalarType expected) const { return kind == expected; }

    ScalarType kind;
    std::size_t layout;
};

struct ValueField {
    std::string name;
    NativeType type;
};

struct ValueLayout {
    ScalarType kind;
    std::string name;
    std::vector<ValueField> fields;
    std::size_t words;
    std::vector<std::string> enumerators = {};
    std::size_t fixedExtent = 0;
};

inline bool isSequence(ScalarType type)
{
    return type == ScalarType::Queue || type == ScalarType::Stack;
}

inline bool usesCollectionHandle(ScalarType type)
{
    return type == ScalarType::List || type == ScalarType::Array || type == ScalarType::HashSet ||
        type == ScalarType::Dictionary || isSequence(type);
}

inline bool isFloating(NativeType type)
{
    return type == ScalarType::Float || type == ScalarType::Float32;
}

inline bool usesAggregateLayout(NativeType type)
{
    return usesCollectionHandle(type.kind) || type == ScalarType::Result ||
        type == ScalarType::Record || type == ScalarType::Pair || type == ScalarType::Optional ||
        type == ScalarType::ResultInterface ||
        type == ScalarType::FixedArray;
}

inline std::size_t valueWords(NativeType type, std::span<const ValueLayout> layouts)
{
    if (usesAggregateLayout(type))
        return layouts[type.layout].words;
    if (type == ScalarType::Text || type == ScalarType::Error)
        return 2;
    if (type == ScalarType::ByteBuffer)
        return 3;
    return 1;
}

inline bool containsReceiverSets(NativeType type, std::span<const ValueLayout> layouts)
{
    if (type == ScalarType::HashSet)
        return true;
    if (type != ScalarType::Record)
        return false;
    for (const auto& field : layouts[type.layout].fields) {
        if (containsReceiverSets(field.type, layouts))
            return true;
    }
    return false;
}

inline bool containsSourceReferences(NativeType type, std::span<const ValueLayout> layouts)
{
    if (type == ScalarType::Receiver || type == ScalarType::ClassReference || type == ScalarType::Interface)
        return true;
    if (!usesAggregateLayout(type))
        return false;
    for (const auto& field : layouts[type.layout].fields) {
        if (containsSourceReferences(field.type, layouts))
            return true;
    }
    return false;
}

inline bool isReceiverValue(NativeType type)
{
    switch (type.kind) {
    case ScalarType::Array:
    case ScalarType::List:
    case ScalarType::Dictionary:
    case ScalarType::Queue:
    case ScalarType::Stack:
    case ScalarType::Optional:
    case ScalarType::Result:
    case ScalarType::ResultInterface:
    case ScalarType::Pair:
    case ScalarType::Error:
        return true;
    default:
        return false;
    }
}

inline bool fixedArrayHasStaticOwnership(NativeType type, std::span<const ValueLayout> layouts)
{
    switch (type.kind) {
    case ScalarType::Integer:
    case ScalarType::UInt32:
    case ScalarType::Boolean:
    case ScalarType::Enum:
    case ScalarType::Float:
    case ScalarType::Float32:
        return true;
    case ScalarType::FixedArray: {
        const auto& array = layouts[type.layout];
        // Numeric elements have no variable ownership cost.
        return array.fixedExtent == 0 || fixedArrayHasStaticOwnership(array.fields.front().type, layouts);
    }
    default:
        return false;
    }
}

inline bool containsReceiverValues(NativeType type, std::span<const ValueLayout> layouts)
{
    if (type == ScalarType::FixedArray)
        return !fixedArrayHasStaticOwnership(type, layouts);
    if (isReceiverValue(type))
        return true;
    if (type != ScalarType::Record)
        return false;
    for (const auto& field : layouts[type.layout].fields) {
        if (containsReceiverValues(field.type, layouts))
            return true;
    }
    return false;
}

}
