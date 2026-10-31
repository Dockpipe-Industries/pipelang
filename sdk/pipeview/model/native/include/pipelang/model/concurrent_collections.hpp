// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "extended_collections.hpp"

#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace pipelang::model {

namespace concurrent_detail {

template<class Collection>
struct Cell {
    std::mutex mutex;
    Collection collection;
};

}

// Handles share identity and lifetime. A mutex protects each complete operation;
// these collections do not promise lock-free progress or multi-call transactions.
// Source admission excludes shared handles from ordinary owned-value containers.
template<class K, class V>
class ConcurrentDictionary {
    static_assert(collections_detail::supportedKey<K>,
                  "concurrent dictionary requires int, bool, string or enum keys");
    static_assert(std::is_nothrow_move_constructible_v<V>,
                  "concurrent dictionary values require nonthrowing moves");
    using Cell = concurrent_detail::Cell<Dictionary<K, V>>;

public:
    using Entry = KeyValuePair<K, V>;

    ConcurrentDictionary() : cell_(std::make_shared<Cell>()) {
    }

    static ConcurrentDictionary Empty() {
        return {};
    }

    bool tryAdd(K key, V value) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        if (cell.collection.containsKey(key))
            return false;
        cell.collection.add(std::move(key), std::move(value));
        return true;
    }

    void set(K key, V value) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.set(std::move(key), std::move(value));
    }

    V at(const K& key) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.at(key);
    }

    Optional<V> tryGet(const K& key) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.tryGet(key);
    }

    bool containsKey(const K& key) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.containsKey(key);
    }

    Optional<V> tryRemove(const K& key) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        // Own the return payload before publication of the removal. Its move
        // cannot throw, including when NRVO is unavailable.
        auto result = cell.collection.tryGet(key);
        if (result.has_value())
            cell.collection.remove(key);
        return result;
    }

    V getOrAdd(K key, V value) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        if (cell.collection.containsKey(key))
            return cell.collection.at(key);
        V result(value);
        cell.collection.add(std::move(key), std::move(value));
        return result;
    }

    bool tryUpdate(K key, V replacement, V comparison) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        auto current = cell.collection.tryGet(key);
        if (!current.has_value() || !(current.value() == comparison))
            return false;
        cell.collection.set(std::move(key), std::move(replacement));
        return true;
    }

    std::int64_t count() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.count();
    }

    void clear() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.clear();
    }

    List<K> keys() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.keys();
    }

    List<V> mappedValues() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.mappedValues();
    }

    List<Entry> entries() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.entries();
    }

    std::vector<Entry> values() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.values();
    }

    void requireCell() const {
        (void)checkedCell();
    }

private:
    Cell& checkedCell() const {
        if (!cell_)
            throw ExecutionFailure("concurrent dictionary handle has no cell");
        return *cell_;
    }

    std::shared_ptr<Cell> cell_;
};

template<class T>
class ConcurrentQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "concurrent queue values require nonthrowing moves");
    using Cell = concurrent_detail::Cell<Queue<T>>;

public:
    ConcurrentQueue() : cell_(std::make_shared<Cell>()) {
    }

    static ConcurrentQueue Empty() {
        return {};
    }

    void enqueue(T value) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.enqueue(std::move(value));
    }

    Optional<T> tryDequeue() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        // The ordinary owner copies its result before removing the item. Only
        // nonthrowing moves occur after removal, including Optional wrapping.
        return cell.collection.tryDequeue();
    }

    Optional<T> tryPeek() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        if (cell.collection.count() == 0)
            return Optional<T>::None();
        return Optional<T>::Some(cell.collection.peek());
    }

    std::int64_t count() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.count();
    }

    void clear() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.clear();
    }

    List<T> items() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        auto result = List<T>::Empty();
        for (const auto& value : cell.collection.values())
            result.add(value);
        return result;
    }

    std::vector<T> values() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.values();
    }

    void requireCell() const {
        (void)checkedCell();
    }

private:
    Cell& checkedCell() const {
        if (!cell_)
            throw ExecutionFailure("concurrent queue handle has no cell");
        return *cell_;
    }

    std::shared_ptr<Cell> cell_;
};

template<class T>
class ConcurrentStack {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "concurrent stack values require nonthrowing moves");
    using Cell = concurrent_detail::Cell<Stack<T>>;

public:
    ConcurrentStack() : cell_(std::make_shared<Cell>()) {
    }

    static ConcurrentStack Empty() {
        return {};
    }

    void push(T value) const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.push(std::move(value));
    }

    Optional<T> tryPop() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.tryPop();
    }

    Optional<T> tryPeek() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        if (cell.collection.count() == 0)
            return Optional<T>::None();
        return Optional<T>::Some(cell.collection.peek());
    }

    std::int64_t count() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.count();
    }

    void clear() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        cell.collection.clear();
    }

    List<T> items() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        auto result = List<T>::Empty();
        for (const auto& value : cell.collection.values())
            result.add(value);
        return result;
    }

    std::vector<T> values() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.collection.values();
    }

    void requireCell() const {
        (void)checkedCell();
    }

private:
    Cell& checkedCell() const {
        if (!cell_)
            throw ExecutionFailure("concurrent stack handle has no cell");
        return *cell_;
    }

    std::shared_ptr<Cell> cell_;
};

template<class K, class V>
void validate(const ConcurrentDictionary<K, V>& value) {
    value.requireCell();
}

template<class T>
void validate(const ConcurrentQueue<T>& value) {
    value.requireCell();
}

template<class T>
void validate(const ConcurrentStack<T>& value) {
    value.requireCell();
}

}
