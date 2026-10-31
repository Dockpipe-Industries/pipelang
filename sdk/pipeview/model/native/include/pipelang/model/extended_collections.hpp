// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "collection.hpp"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <unordered_map>

namespace pipelang::model {

template<class T> class Array;
template<class K, class V> struct KeyValuePair;
template<class K, class V> class Dictionary;
template<class T> class HashSet;
template<class T> class Queue;
template<class T> class Stack;

// Declare every overload before dependent recursion. ADL also finds generated
// record overloads in the record's own namespace. The compiler must reject
// service, reference, task and cancellation payloads recursively.
template<class T> std::size_t ownedBytes(const Array<T>& value);
template<class T> std::size_t ownedElements(const Array<T>& value);
template<class T> void validate(const Array<T>& value);
template<class T> std::size_t ownedBytes(const HashSet<T>& value);
template<class T> std::size_t ownedElements(const HashSet<T>& value);
template<class T> void validate(const HashSet<T>& value);
template<class T> std::size_t ownedBytes(const Queue<T>& value);
template<class T> std::size_t ownedElements(const Queue<T>& value);
template<class T> void validate(const Queue<T>& value);
template<class T> std::size_t ownedBytes(const Stack<T>& value);
template<class T> std::size_t ownedElements(const Stack<T>& value);
template<class T> void validate(const Stack<T>& value);

template<class K, class V> std::size_t ownedBytes(const KeyValuePair<K,V>& value);
template<class K, class V> std::size_t ownedElements(const KeyValuePair<K,V>& value);
template<class K, class V> void validate(const KeyValuePair<K,V>& value);
template<class K, class V> std::size_t ownedBytes(const Dictionary<K,V>& value);
template<class K, class V> std::size_t ownedElements(const Dictionary<K,V>& value);
template<class K, class V> void validate(const Dictionary<K,V>& value);

template<class K, class V>
struct KeyValuePair {
    K key{};
    V value{};
    bool operator==(const KeyValuePair&) const = default;

};
template<class K, class V>
std::size_t ownedBytes(const KeyValuePair<K,V>& value) {
    using pipelang::model::ownedBytes;
    auto bytes = boundedOwnedSum(sizeof(value), ownedBytes(value.key), maximumListBytes);
    return boundedOwnedSum(bytes, ownedBytes(value.value), maximumListBytes);
}

template<class K, class V>
std::size_t ownedElements(const KeyValuePair<K,V>& value) {
    using pipelang::model::ownedElements;
    return boundedOwnedSum(ownedElements(value.key), ownedElements(value.value), maximumListElements);
}

template<class K, class V>
void validate(const KeyValuePair<K,V>& value) {
    using pipelang::model::validate;
    validate(value.key);
    validate(value.value);
}

namespace collections_detail {
struct Ownership {
    std::size_t bytes;
    std::size_t elements;
};
template<class T>
Ownership elementOwnership(const T& value) {
    using pipelang::model::ownedBytes;
    using pipelang::model::ownedElements;
    return {ownedBytes(value), boundedOwnedSum(1, ownedElements(value), maximumListElements)};
}

inline Ownership appendOwnership(Ownership current, Ownership added) {
    Ownership result{
        boundedOwnedSum(current.bytes, added.bytes, maximumListBytes),
        boundedOwnedSum(current.elements, added.elements, maximumListElements)};
    if (result.elements > maximumListElements)
        throw ExecutionFailure("collection aggregate element limit exceeded");
    if (result.bytes > maximumListBytes)
        throw ExecutionFailure("collection owned byte limit exceeded");
    return result;
}

inline std::size_t indexOf(std::int64_t index, std::size_t size) {
    if (index < 0 || static_cast<std::uint64_t>(index) >= size)
        throw ExecutionFailure("collection index outside admitted range");
    return static_cast<std::size_t>(index);
}

template<class T>
inline constexpr bool supportedKey = std::is_same_v<T, std::int64_t> ||
    std::is_same_v<T, bool> || std::is_same_v<T, std::string> || std::is_enum_v<T>;

// Immutable internal nodes may share storage between semantic value copies.
// No mutable alias is exposed. Metrics count each copy's logical ownership;
// allocator capacity, hash buckets and node bookkeeping are implementation cost.
template<class T>
class Sequence {
public:
    static_assert(std::is_nothrow_move_constructible_v<T>,
        "compiler-admitted owned values require nonthrowing moves");
    explicit Sequence(std::size_t baseBytes) : baseBytes_(baseBytes), ownership_{baseBytes, 0} {
    }

    Sequence(const Sequence&) = default;

    Sequence(Sequence&& other) noexcept
        : baseBytes_(other.baseBytes_), ownership_(other.ownership_),
          items_(std::move(other.items_)), head_(other.head_) {
        other.items_.clear();
        other.head_ = 0;
        other.ownership_ = {other.baseBytes_, 0};
    }

    Sequence& operator=(Sequence other) noexcept {
        using std::swap;
        swap(baseBytes_, other.baseBytes_);
        swap(ownership_, other.ownership_);
        items_.swap(other.items_);
        swap(head_, other.head_);
        return *this;
    }

    void append(T value) {
        using pipelang::model::validate;
        validate(value);
        const auto next = appendOwnership(ownership_, elementOwnership(value));
        auto owned = std::make_shared<const T>(std::move(value));
        if (head_ != 0 && head_ >= items_.size() / 2) {
            items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
        items_.push_back(std::move(owned));
        ownership_ = next;
    }

    void replace(std::size_t index, T value) {
        using pipelang::model::validate;
        validate(value);
        const auto previous = elementOwnership(*items_[head_ + index]);
        const Ownership remainder{ownership_.bytes - previous.bytes, ownership_.elements - previous.elements};
        const auto next = appendOwnership(remainder, elementOwnership(value));
        auto owned = std::make_shared<const T>(std::move(value));
        items_[head_ + index].swap(owned);
        ownership_ = next;
    }

    T at(std::size_t index) const {
        return *items_[head_ + index];
    }

    T take(bool front) {
        if (size() == 0)
            throw ExecutionFailure("collection is empty");
        const auto& owned = front ? items_[head_] : items_.back();
        T result = *owned;
        const auto previous = elementOwnership(*owned);
        if (front) {
            items_[head_].reset();
            ++head_;
        } else {
            items_.pop_back();
        }
        if (head_ == items_.size()) {
            items_.clear();
            head_ = 0;
        }
        ownership_.bytes -= previous.bytes;
        ownership_.elements -= previous.elements;
        return result;
    }

    void clear() noexcept {
        items_.clear();
        head_ = 0;
        ownership_ = {baseBytes_, 0};
    }

    std::size_t size() const noexcept {
        return items_.size() - head_;
    }

    Ownership ownership() const noexcept {
        return ownership_;
    }

    std::vector<T> snapshot(bool reverse = false) const {
        std::vector<T> result;
        result.reserve(size());
        if (reverse) {
            for (std::size_t index = size(); index != 0; --index)
                result.push_back(at(index - 1));
        } else {
            for (std::size_t index = 0; index < size(); ++index)
                result.push_back(at(index));
        }
        return result;
    }

    bool operator==(const Sequence& other) const {
        if (size() != other.size())
            return false;
        for (std::size_t index = 0; index < size(); ++index) {
            if (!(*items_[head_ + index] == *other.items_[other.head_ + index]))
                return false;
        }
        return true;
    }

private:
    std::size_t baseBytes_;
    Ownership ownership_;
    std::vector<std::shared_ptr<const T>> items_;
    std::size_t head_ = 0;
};
}

template<class T>
class Array {
public:
    Array() : items_(sizeof(Array)) {
    }

    static Array Create(std::int64_t size, T initial) {
        if (size < 0 || size > static_cast<std::int64_t>(maximumListElements))
            throw ExecutionFailure("array size outside admitted range");
        using pipelang::model::validate;
        validate(initial);
        Array result;
        for (std::int64_t index = 0; index < size; ++index)
            result.items_.append(initial);
        return result;
    }

    std::int64_t count() const {
        return static_cast<std::int64_t>(items_.size());
    }

    T at(std::int64_t index) const {
        return items_.at(collections_detail::indexOf(index, items_.size()));
    }

    void set(std::int64_t index, T value) {
        items_.replace(collections_detail::indexOf(index, items_.size()), std::move(value));
    }

    std::vector<T> values() const {
        return items_.snapshot();
    }

    std::size_t ownedByteCost() const {
        return items_.ownership().bytes;
    }

    std::size_t ownedElementCost() const {
        return items_.ownership().elements;
    }

    bool operator==(const Array&) const = default;

private:
    collections_detail::Sequence<T> items_;
};

template<class K, class V>
class Dictionary {
public:
    static_assert(collections_detail::supportedKey<K>, "dictionary requires int, bool, string or enum keys");
    using Entry = KeyValuePair<K,V>;
    Dictionary() = default;

    Dictionary(const Dictionary&) = default;

    Dictionary(Dictionary&& other) noexcept
        : entries_(std::move(other.entries_)), index_(std::move(other.index_)),
          ownership_(other.ownership_) {
        other.entries_.clear();
        other.index_.clear();
        other.ownership_ = {sizeof(Dictionary), 0};
    }

    Dictionary& operator=(Dictionary other) noexcept {
        entries_.swap(other.entries_);
        index_.swap(other.index_);
        using std::swap;
        swap(ownership_, other.ownership_);
        return *this;
    }

    static Dictionary Empty() {
        return {};
    }

    void add(K key, V value) {
        using pipelang::model::validate;
        validate(key);
        validate(value);
        if (index_.contains(key))
            throw ExecutionFailure("dictionary key already exists");
        auto entry = std::make_shared<const Entry>(Entry{std::move(key), std::move(value)});
        const auto next = collections_detail::appendOwnership(ownership_, collections_detail::elementOwnership(*entry));
        if (entries_.size() == entries_.capacity()) {
            const auto capacity = entries_.capacity() == 0 ? 1 : entries_.capacity() * 2;
            entries_.reserve(std::min(capacity, maximumListElements));
        }
        // After reserve, appending a shared_ptr cannot throw. Hash insertion has
        // the standard strong guarantee for the supported nonthrowing hash/equal
        // key types. No index/sequence mismatch is observable on failure.
        index_.emplace(entry->key, entries_.size());
        entries_.push_back(std::move(entry));
        ownership_ = next;
    }

    void set(K key, V value) {
        using pipelang::model::validate;
        validate(key);
        auto position = index_.find(key);
        if (position == index_.end()) {
            add(std::move(key), std::move(value));
            return;
        }
        using pipelang::model::validate;
        validate(key);
        validate(value);
        auto entry = std::make_shared<const Entry>(Entry{std::move(key), std::move(value)});
        auto& slot = entries_[position->second];
        const auto previous = collections_detail::elementOwnership(*slot);
        const collections_detail::Ownership remainder{ownership_.bytes - previous.bytes, ownership_.elements - previous.elements};
        const auto next = collections_detail::appendOwnership(remainder, collections_detail::elementOwnership(*entry));
        slot.swap(entry);
        ownership_ = next;
    }

    V at(const K& key) const {
        using pipelang::model::validate;
        validate(key);
        const auto position = index_.find(key);
        if (position == index_.end())
            throw ExecutionFailure("dictionary key was not found");
        return entries_[position->second]->value;
    }

    Optional<V> tryGet(const K& key) const {
        using pipelang::model::validate;
        validate(key);
        const auto position = index_.find(key);
        if (position == index_.end())
            return Optional<V>::None();
        return Optional<V>::Some(entries_[position->second]->value);
    }

    bool containsKey(const K& key) const {
        using pipelang::model::validate;
        validate(key);
        return index_.contains(key);
    }

    bool remove(const K& key) {
        using pipelang::model::validate;
        validate(key);
        auto position = index_.find(key);
        if (position == index_.end())
            return false;
        const auto slot = position->second;
        const auto previous = collections_detail::elementOwnership(*entries_[slot]);
        index_.erase(position);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(slot));
        for (std::size_t index = slot; index < entries_.size(); ++index)
            index_.find(entries_[index]->key)->second = index;
        ownership_.bytes -= previous.bytes;
        ownership_.elements -= previous.elements;
        return true;
    }

    std::int64_t count() const {
        return static_cast<std::int64_t>(entries_.size());
    }

    std::vector<Entry> values() const {
        std::vector<Entry> result;
        result.reserve(entries_.size());
        for (const auto& entry : entries_)
            result.push_back(*entry);
        return result;
    }

    List<Entry> entries() const {
        auto result = List<Entry>::Empty();
        for (const auto& entry : entries_)
            result.add(*entry);
        return result;
    }

    List<K> keys() const {
        auto result = List<K>::Empty();
        for (const auto& entry : entries_)
            result.add(entry->key);
        return result;
    }

    List<V> mappedValues() const {
        auto result = List<V>::Empty();
        for (const auto& entry : entries_)
            result.add(entry->value);
        return result;
    }

    void clear() noexcept {
        index_.clear();
        entries_.clear();
        ownership_ = {sizeof(Dictionary), 0};
    }

    std::size_t ownedByteCost() const {
        return ownership_.bytes;
    }

    std::size_t ownedElementCost() const {
        return ownership_.elements;
    }

    bool operator==(const Dictionary& other) const {
        if (entries_.size() != other.entries_.size())
            return false;
        for (const auto& entry : entries_) {
            const auto otherPosition = other.index_.find(entry->key);
            if (otherPosition == other.index_.end() ||
                !(entry->value == other.entries_[otherPosition->second]->value))
                return false;
        }
        return true;
    }

private:
    std::vector<std::shared_ptr<const Entry>> entries_;
    std::unordered_map<K, std::size_t> index_;
    collections_detail::Ownership ownership_{sizeof(Dictionary), 0};
};

template<class T>
class HashSet {
public:
    static HashSet Empty() {
        return {};
    }

    bool add(T value) {
        if (items_.containsKey(value))
            return false;
        const KeyValuePair<T,bool> candidate{value, false};
        collections_detail::appendOwnership(
            {ownedByteCost(), ownedElementCost()}, collections_detail::elementOwnership(candidate));
        items_.add(std::move(value), false);
        return true;
    }

    bool remove(const T& value) {
        return items_.remove(value);
    }

    bool contains(const T& value) const {
        return items_.containsKey(value);
    }

    std::int64_t count() const {
        return items_.count();
    }

    void clear() noexcept {
        items_.clear();
    }

    std::vector<T> values() const {
        std::vector<T> result;
        result.reserve(static_cast<std::size_t>(count()));
        for (const auto& entry : items_.values())
            result.push_back(entry.key);
        return result;
    }

    List<T> items() const {
        auto result = List<T>::Empty();
        for (const auto& value : values())
            result.add(value);
        return result;
    }

    std::size_t ownedByteCost() const {
        return boundedOwnedSum(sizeof(HashSet), items_.ownedByteCost(), maximumListBytes);
    }

    std::size_t ownedElementCost() const {
        return items_.ownedElementCost();
    }

    bool operator==(const HashSet&) const = default;

private:
    Dictionary<T,bool> items_;
};

template<class T>
class Queue {
public:
    Queue() : items_(sizeof(Queue)) {
    }

    static Queue Empty() {
        return {};
    }

    void enqueue(T value) {
        items_.append(std::move(value));
    }

    T dequeue() {
        return items_.take(true);
    }

    Optional<T> tryDequeue() {
        if (items_.size() == 0)
            return Optional<T>::None();
        return Optional<T>::Some(items_.take(true));
    }

    T peek() const {
        return items_.at(collections_detail::indexOf(0, items_.size()));
    }

    void clear() noexcept {
        items_.clear();
    }

    std::int64_t count() const {
        return static_cast<std::int64_t>(items_.size());
    }

    std::vector<T> values() const {
        return items_.snapshot();
    }

    std::size_t ownedByteCost() const {
        return items_.ownership().bytes;
    }

    std::size_t ownedElementCost() const {
        return items_.ownership().elements;
    }

    bool operator==(const Queue&) const = default;

private:
    collections_detail::Sequence<T> items_;
};

template<class T>
class Stack {
public:
    Stack() : items_(sizeof(Stack)) {
    }

    static Stack Empty() {
        return {};
    }

    void push(T value) {
        items_.append(std::move(value));
    }

    T pop() {
        return items_.take(false);
    }

    Optional<T> tryPop() {
        if (items_.size() == 0)
            return Optional<T>::None();
        return Optional<T>::Some(items_.take(false));
    }

    T peek() const {
        if (items_.size() == 0)
            throw ExecutionFailure("collection is empty");
        return items_.at(items_.size() - 1);
    }

    void clear() noexcept {
        items_.clear();
    }

    std::int64_t count() const {
        return static_cast<std::int64_t>(items_.size());
    }

    std::vector<T> values() const {
        return items_.snapshot(true);
    }

    std::size_t ownedByteCost() const {
        return items_.ownership().bytes;
    }

    std::size_t ownedElementCost() const {
        return items_.ownership().elements;
    }

    bool operator==(const Stack&) const = default;

private:
    collections_detail::Sequence<T> items_;
};

// Public collection overloads recurse through immutable snapshots. Element cost
// caches are invariant under the only public mutation operations.
template<class C>
void validateExtendedCollection(const C& value) {
    using pipelang::model::validate;
    for (const auto& item : value.values())
        validate(item);
    if (value.ownedByteCost() > maximumListBytes || value.ownedElementCost() > maximumListElements)
        throw ExecutionFailure("collection ownership budget exceeded");
}

template<class T>
std::size_t ownedBytes(const Array<T>& value) {
    return value.ownedByteCost();
}

template<class T>
std::size_t ownedElements(const Array<T>& value) {
    return value.ownedElementCost();
}

template<class T>
void validate(const Array<T>& value) {
    validateExtendedCollection(value);
}

template<class T>
std::size_t ownedBytes(const HashSet<T>& value) {
    return value.ownedByteCost();
}

template<class T>
std::size_t ownedElements(const HashSet<T>& value) {
    return value.ownedElementCost();
}

template<class T>
void validate(const HashSet<T>& value) {
    validateExtendedCollection(value);
}

template<class T>
std::size_t ownedBytes(const Queue<T>& value) {
    return value.ownedByteCost();
}

template<class T>
std::size_t ownedElements(const Queue<T>& value) {
    return value.ownedElementCost();
}

template<class T>
void validate(const Queue<T>& value) {
    validateExtendedCollection(value);
}

template<class T>
std::size_t ownedBytes(const Stack<T>& value) {
    return value.ownedByteCost();
}

template<class T>
std::size_t ownedElements(const Stack<T>& value) {
    return value.ownedElementCost();
}

template<class T>
void validate(const Stack<T>& value) {
    validateExtendedCollection(value);
}

template<class K, class V>
std::size_t ownedBytes(const Dictionary<K,V>& value) {
    return value.ownedByteCost();
}

template<class K, class V>
std::size_t ownedElements(const Dictionary<K,V>& value) {
    return value.ownedElementCost();
}

template<class K, class V>
void validate(const Dictionary<K,V>& value) {
    validateExtendedCollection(value);
}
}
