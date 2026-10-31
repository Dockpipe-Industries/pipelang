// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "object.hpp"

#include <exception>
#include <type_traits>
#include <utility>

namespace pipelang::model {

// Construction may temporarily contain default edges, but a successfully
// constructed reference-bearing value cannot hide an uninitialized reference.
template<class Value>
struct ObjectRequired {
    static void validate(const typename ObjectStorage<Value>::Stored&) {
    }
};

template<class Tag>
struct ObjectRequired<ObjectRef<Tag>> {
    static void validate(const ObjectEdge<Tag>& value) {
        if (value.id.isNull())
            throw ExecutionFailure("constructor must initialize every object reference");
    }
};

template<class T>
struct ObjectRequired<Optional<T>> {
    static void validate(const typename ObjectStorage<Optional<T>>::Stored& value) {
        if (value.has_value())
            ObjectRequired<T>::validate(value.value());
    }
};

template<template<class> class Sequence, class T>
struct ObjectRequiredSequence {
    static void validate(const typename ObjectStorage<Sequence<T>>::Stored& value) {
        for (const auto& item : value.values())
            ObjectRequired<T>::validate(item);
    }
};

template<class T>
struct ObjectRequired<List<T>> : ObjectRequiredSequence<List, T> {
};
template<class T>
struct ObjectRequired<Array<T>> : ObjectRequiredSequence<Array, T> {
};
template<class T>
struct ObjectRequired<Queue<T>> : ObjectRequiredSequence<Queue, T> {
};
template<class T>
struct ObjectRequired<Stack<T>> : ObjectRequiredSequence<Stack, T> {
};

template<class K, class V>
struct ObjectRequired<Dictionary<K, V>> {
    static void validate(const typename ObjectStorage<Dictionary<K, V>>::Stored& value) {
        for (const auto& entry : value.values())
            ObjectRequired<V>::validate(entry.value);
    }
};

template<class T, class E>
struct ObjectRequired<Result<T, E>> {
    static void validate(const typename ObjectStorage<Result<T, E>>::Stored& value) {
        if (value.is_success())
            ObjectRequired<T>::validate(value.value());
    }
};

template<class E>
struct ObjectRequired<Result<void, E>> {
    static void validate(const typename ObjectStorage<Result<void, E>>::Stored&) {
    }
};

template<class K, class V>
struct ObjectRequired<KeyValuePair<K, V>> {
    static void validate(const typename ObjectStorage<KeyValuePair<K, V>>::Stored& value) {
        ObjectRequired<K>::validate(value.key);
        ObjectRequired<V>::validate(value.value);
    }
};

// One publication boundary for a model's owned state and its ordinary object
// graph. Existing owned-only models continue using Transaction<T> directly.
template<class State>
class GraphModelTransaction {
public:
    GraphModelTransaction(State& target, std::shared_ptr<ObjectDomain> domain)
        : target_(target), root_(target), graph_(std::move(domain)) {
    }

    State& state() noexcept {
        return root_.state();
    }

    const State& state() const noexcept {
        return root_.state();
    }

    GraphTransaction& graph() noexcept {
        return graph_;
    }

    void commit() {
        using pipelang::model::validate;
        validate(root_.state());
        graph_.prepareCommit();
        // Root validation, graph cloning and tracing precede publication.
        // Transaction's only publication operation is a nonthrowing state swap.
        graph_.commit([this]() noexcept {
            root_.commit();
        });
    }

    void refresh() {
        State refreshed(target_);
        graph_.refresh();
        static_assert(std::is_nothrow_swappable_v<State>);
        using std::swap;
        swap(root_.state(), refreshed);
    }

    ObjectExecution execution(TaskScope& scope, EvaluationBudget& budget) {
        auto callbacks = std::make_shared<ObjectRootCallbacks>();
        callbacks->context = this;
        callbacks->commit = [](void* context) {
            static_cast<GraphModelTransaction*>(context)->commit();
        };
        callbacks->refresh = [](void* context) {
            static_cast<GraphModelTransaction*>(context)->refresh();
        };
        return ObjectExecution(scope, budget, graph_, std::move(callbacks));
    }

private:
    State& target_;
    Transaction<State> root_;
    GraphTransaction graph_;
};

// Unlike owned-only publication, a graph commit may fail during preparation.
// Keep that failure inside coroutine exception handling, before final publication.
template<class Commit, class Abandoning>
class ObjectCommitOnReturn {
public:
    ObjectCommitOnReturn(Commit commit, Abandoning abandoning)
        : commit_(std::move(commit)), abandoning_(std::move(abandoning)),
          exceptions_(std::uncaught_exceptions()) {
    }

    ObjectCommitOnReturn(const ObjectCommitOnReturn&) = delete;
    ObjectCommitOnReturn& operator=(const ObjectCommitOnReturn&) = delete;

    ~ObjectCommitOnReturn() noexcept(false) {
        if (armed_ && !abandoning_() && std::uncaught_exceptions() == exceptions_)
            commit_();
    }

    void arm() noexcept {
        armed_ = true;
    }

private:
    Commit commit_;
    Abandoning abandoning_;
    int exceptions_;
    bool armed_ = false;
};

}
