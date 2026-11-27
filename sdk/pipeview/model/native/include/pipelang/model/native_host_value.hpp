// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "atomic.hpp"
#include "concurrent_collections.hpp"
#include "result.hpp"
#include "synchronization.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pipelang::native_app_v19 {
namespace detail {
class HostedSequenceCell;
class HostedDictionaryCell;
}
template<class Item>
struct HostedValueCodec;

class HostedProjection;
class HostedInput;

// Immutable, owned value. String views borrow from this value or a copy of it.
using HostedAtomic = std::variant<model::Atomic<std::int64_t>, model::Atomic<bool>, model::Atomic<double>, model::Atomic<float>>;

class HostedValue {
public:
    enum class Kind {
        Void, Integer, Boolean, Text, Error, Record, Result, List, Enum, Optional, HashSet, UInt32, Float, Float32,
        ByteBuffer, Queue, Stack, Dictionary, ResultInterface, ObjectReference, Semaphore, ConcurrentIntegerQueue,
        Lock, Atomic, ConcurrentSequence, ConcurrentDictionary
    };

    HostedValue() = default;
    static HostedValue fromInteger(std::int64_t value);
    static HostedValue fromUInt32(std::uint32_t value);
    static HostedValue fromFloat(double value);
    static HostedValue fromFloat32(float value);
    static HostedValue fromBytes(std::span<const std::uint8_t> value);
    static HostedValue fromBoolean(bool value);
    static HostedValue fromSemaphore(model::SemaphoreSlim semaphore);
    static HostedValue fromConcurrentQueue(model::ConcurrentQueue<std::int64_t> queue);
    template<class Item>
    static HostedValue fromConcurrentQueue(model::ConcurrentQueue<Item> queue, HostedValueCodec<Item> codec);
    template<class Item>
    static HostedValue fromConcurrentStack(model::ConcurrentStack<Item> stack, HostedValueCodec<Item> codec);
    template<class Key, class Value>
    static HostedValue fromConcurrentDictionary(model::ConcurrentDictionary<Key, Value> dictionary,
                                                HostedValueCodec<Key> key, HostedValueCodec<Value> value);
    static HostedValue fromLock(model::Lock lock);
    static HostedValue fromAtomic(HostedAtomic atomic);
    static HostedValue fromText(std::string value);
    static HostedValue fromError(std::string message);
    static HostedValue fromRecord(std::string type, std::vector<std::pair<std::string, HostedValue>> fields);
    static HostedValue fromEnum(std::string type, std::int64_t ordinal, std::string member);
    static HostedValue fromList(std::string type, std::vector<HostedValue> items);
    static HostedValue fromHashSet(std::string type, std::vector<HostedValue> items);
    static HostedValue fromQueue(std::string type, std::vector<HostedValue> items);
    static HostedValue fromStack(std::string type, std::vector<HostedValue> topFirst);
    static HostedValue fromDictionary(std::string type, std::vector<HostedValue> entries);
    static HostedValue fromOptional(std::string type, HostedValue value = {});
    static HostedValue fromResult(std::string type, model::Outcome outcome, HostedValue value = {});

    Kind kind() const;
    std::string_view typeName() const;
    std::int64_t integer() const;
    std::uint32_t uint32() const;
    double floating() const;
    float float32() const;
    std::span<const std::uint8_t> bytes() const;
    bool boolean() const;
    model::SemaphoreSlim semaphore() const;
    model::ConcurrentQueue<std::int64_t> concurrentQueue() const;
    model::Lock lock() const;
    HostedAtomic atomic() const;
    std::string_view text() const;
    std::int64_t ordinal() const;
    std::string_view enumerator() const;
    HostedValue field(std::string_view name) const;
    std::size_t size() const;
    HostedValue at(std::size_t index) const;
    bool hasValue() const;
    model::Outcome outcome() const;
    HostedValue value() const;
    HostedValue error() const;
    bool sameObject(const HostedValue& other) const;

private:
    struct Node;
    struct ExportedObjectNode;
    struct SemaphoreNode;
    struct ConcurrentIntegerQueueNode;
    struct ConcurrentSequenceNode;
    struct ConcurrentDictionaryNode;
    struct LockNode;
    struct AtomicNode;
    friend class HostedProjection;
    friend class HostedInput;
    friend class HostedComposition;
    friend class NativeConcurrentSequences;
    friend class NativeConcurrentDictionaries;
    static HostedValue fromHostedDictionary(std::string type, std::shared_ptr<const detail::HostedDictionaryCell> dictionary);
    std::shared_ptr<const detail::HostedDictionaryCell> hostedDictionary() const;
    static HostedValue fromHostedSequence(std::string type, std::shared_ptr<const detail::HostedSequenceCell> sequence);
    std::shared_ptr<const detail::HostedSequenceCell> hostedSequence() const;
    bool containsObjectReference() const;

    explicit HostedValue(std::shared_ptr<const Node> node);
    const Node& require(Kind expected) const;
    const Node& requireResult() const;
    const std::vector<HostedValue>& collectionItems() const;
    static HostedValue fromChildren(Kind kind, std::string type, std::vector<HostedValue> children);

    std::shared_ptr<const Node> node_;
};

template<class Item>
struct HostedValueCodec {
    std::string type;
    std::function<HostedValue(const Item&)> encode;
    std::function<Item(const HostedValue&)> decode;

    HostedValue project(const Item& item) const
    {
        auto value = encode(item);
        if (value.kind() == HostedValue::Kind::Void)
            throw model::ExecutionFailure("host collection codec encoded a void value");
        return value;
    }
};

}

#include "detail/native_host_sequence_impl.hpp"
#include "detail/native_host_dictionary_impl.hpp"
