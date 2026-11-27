// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "extended_collections.hpp"
#include "task.hpp"
#include "source_call.hpp"

#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace pipelang::model {

struct ObjectId {
    std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t generation = 0;

    bool isNull() const noexcept {
        return slot == std::numeric_limits<std::uint32_t>::max();
    }

    auto operator<=>(const ObjectId&) const = default;
};

class ObjectTrace {
public:
    void edge(ObjectId id) {
        if (edges_.size() >= maximumEdges)
            throw ExecutionFailure("object reference edge limit exceeded");
        edges_.push_back(id);
    }

    const std::vector<ObjectId>& edges() const noexcept {
        return edges_;
    }

    static constexpr std::size_t maximumEdges = 16384;

private:
    std::vector<ObjectId> edges_;
};

class ObjectState {
public:
    virtual ~ObjectState() = default;
    virtual std::unique_ptr<ObjectState> clone() const = 0;
    virtual void trace(ObjectTrace& trace) const = 0;
    virtual std::size_t logicalBytes() const = 0;
    virtual void validate() const = 0;
    virtual void validateInitialized() const {
        validate();
    }
};

class ObjectDomain;
class GraphTransaction;

template<class Tag>
class ObjectRef;

template<class Tag>
struct ObjectEdge {
    ObjectId id;
};

namespace object_detail {

struct RootLease {
    ~RootLease() noexcept;
    std::shared_ptr<ObjectDomain> domain;
    ObjectId id;
};

struct Version {
    std::unique_ptr<ObjectState> state;
    std::vector<ObjectId> edges;
    std::size_t bytes = 0;
};

using VersionPtr = std::shared_ptr<const Version>;
using Changes = std::map<ObjectId, VersionPtr>;

}

class ObjectDomain : public std::enable_shared_from_this<ObjectDomain> {
public:
    static constexpr std::size_t maximumCells = 4096;
    static constexpr std::size_t maximumEdges = ObjectTrace::maximumEdges;
    static constexpr std::size_t maximumBytes = 1024 * 1024;
    static constexpr std::size_t maximumTransactions = 33;

    ObjectDomain();

    ObjectDomain(const ObjectDomain&) = delete;
    ObjectDomain& operator=(const ObjectDomain&) = delete;

    void requireOwner() const;

    void close() noexcept;
    void collect();
    std::size_t liveCount() const;
    std::size_t logicalBytes() const;

    std::uint64_t runtimeType(ObjectId id) const;

    template<class Tag>
    ObjectRef<Tag> root(ObjectId id);

private:
    friend class GraphTransaction;
    friend struct object_detail::RootLease;
    template<class Tag> friend class ObjectRef;

    struct Cell {
        std::uint64_t generation = 1;
        std::uint64_t typeId = 0;
        object_detail::VersionPtr state;
        GraphTransaction* provisional = nullptr;
        std::weak_ptr<object_detail::RootLease> root;
    };

    Cell& checked(ObjectId id);

    const Cell& checked(ObjectId id) const;

    template<class Tag>
    Cell& checkedTag(ObjectId id) {
        auto& cell = checked(id);
        if (!Tag::accepts(cell.typeId))
            throw ExecutionFailure("object does not implement the requested type");
        return cell;
    }

    object_detail::VersionPtr version(std::unique_ptr<ObjectState> state, GraphTransaction* transaction);
    ObjectId allocate(std::uint64_t typeId, object_detail::VersionPtr state,
                      GraphTransaction* transaction);
    void destroy(ObjectId id) noexcept;
    void collect(const std::vector<ObjectId>& extraRoots);
    void ensureCapacity(const object_detail::VersionPtr& candidate);
    void addTransaction(GraphTransaction* transaction);
    void removeTransaction(GraphTransaction* transaction) noexcept;
    std::pair<std::size_t, std::size_t> resourceCost() const;

    std::thread::id owner_;
    std::atomic<bool> rootSetDirty_{true};
    bool closed_ = false;
    // Provisional cells count as live; allocation and matching-generation destruction own updates.
    std::size_t liveCells_ = 0;
    std::array<Cell, maximumCells> cells_{};
    std::array<GraphTransaction*, maximumTransactions> transactions_{};
};

template<class Tag>
class ObjectRef {
public:
    ObjectRef() = default;

    template<class Other>
    explicit ObjectRef(const ObjectRef<Other>& other) : root_(other.root_) {
        requireValid();
    }

    bool isNull() const noexcept {
        return !root_;
    }

    template<class Other>
    ObjectRef<Other> as() const {
        requireValid();
        return ObjectRef<Other>(*this);
    }

    void require() const {
        requireValid();
    }

    ObjectId identity() const {
        requireValid();
        return root_->id;
    }

    std::uint64_t typeId() const {
        requireValid();
        return root_->domain->runtimeType(root_->id);
    }

    const std::shared_ptr<ObjectDomain>& ownerDomain() const {
        requireValid();
        return root_->domain;
    }

    void requireValid() const {
        if (!root_)
            throw ExecutionFailure("object reference is uninitialized");
        root_->domain->requireOwner();
        // Compiler-emitted self-owned metadata certifies a pure type predicate.
        // An inherited marker must not opt a custom derived predicate into this
        // path. Unmarked predicates retain root()'s exact acquisition behavior.
        if constexpr (requires {
            typename Tag::PureObjectTagOwner;
            requires std::is_same_v<Tag, typename Tag::PureObjectTagOwner>;
        }) {
            root_->domain->template checkedTag<Tag>(root_->id);
        } else {
            root_->domain->template root<Tag>(root_->id);
        }
    }

    template<class Other>
    bool operator==(const ObjectRef<Other>& other) const noexcept {
        if (!root_ || !other.root_)
            return !root_ && !other.root_;
        return root_->domain == other.root_->domain && root_->id == other.root_->id;
    }

private:
    friend class ObjectDomain;
    template<class Other> friend class ObjectRef;

    explicit ObjectRef(std::shared_ptr<object_detail::RootLease> root) : root_(std::move(root)) {
    }

    std::shared_ptr<object_detail::RootLease> root_;
};

template<class Tag>
ObjectRef<Tag> ObjectDomain::root(ObjectId id) {
    auto& cell = checkedTag<Tag>(id);
    auto lease = cell.root.lock();
    if (!lease) {
        lease = std::make_shared<object_detail::RootLease>();
        lease->domain = shared_from_this();
        lease->id = id;
        cell.root = lease;
        rootSetDirty_.store(true, std::memory_order_release);
    }
    return ObjectRef<Tag>(std::move(lease));
}

class ObjectOwner {
public:
    ObjectOwner() : domain_(std::make_shared<ObjectDomain>()) {
    }

    ObjectOwner(const ObjectOwner&) = delete;
    ObjectOwner& operator=(const ObjectOwner&) = delete;

    ~ObjectOwner() {
        domain_->close();
    }

    const std::shared_ptr<ObjectDomain>& domain() const noexcept {
        return domain_;
    }

private:
    std::shared_ptr<ObjectDomain> domain_;
};

class GraphTransaction {
public:
    static constexpr std::size_t maximumSavepoints = 64;

    explicit GraphTransaction(std::shared_ptr<ObjectDomain> domain);

    GraphTransaction(const GraphTransaction&) = delete;
    GraphTransaction& operator=(const GraphTransaction&) = delete;

    ~GraphTransaction();

    class Savepoint {
    public:
        explicit Savepoint(GraphTransaction& transaction);

        Savepoint(const Savepoint&) = delete;
        Savepoint& operator=(const Savepoint&) = delete;

        ~Savepoint();

        void commit() noexcept;

    private:
        friend class ObjectDomain;
        friend class GraphTransaction;
        GraphTransaction& transaction_;
        object_detail::Changes saved_;
        std::uint64_t allocated_;
        bool committed_ = false;
    };

    Savepoint savepoint();

    // A checked observation of this transaction's provisional allocations.
    // It grants no access to cells or native pointers and changes no GC state.
    std::uint64_t allocationMark() const;

    template<class Tag>
    bool allocatedSince(const ObjectRef<Tag>& reference, std::uint64_t mark) const {
        const auto id = checkedReference(reference);
        if (mark > nextSerial_)
            throw ExecutionFailure("object allocation mark is invalid");
        for (const auto& allocation : allocated_) {
            if (allocation.id == id && allocation.serial > mark)
                return true;
        }
        return false;
    }

    template<class State, class Tag>
    const State& read(const ObjectRef<Tag>& reference) const {
        const auto id = checkedReference(reference);
        return stateAs<State>(*current(id)->state);
    }

    template<class State, class Tag, class Function>
    void mutate(const ObjectRef<Tag>& reference, Function function) {
        const auto id = checkedReference(reference);
        auto candidate = current(id)->state->clone();
        auto& typed = stateAs<State>(*candidate);
        function(typed);
        auto replacement = domain_->version(std::move(candidate), this);
        domain_->ensureCapacity(replacement);
        changes_.insert_or_assign(id, std::move(replacement));
        markReachabilityChanged();
        prepared_ = false;
    }

    template<class State, class Tag>
    ObjectRef<Tag> create(Tag, State initial = {}) {
        domain_->requireOwner();
        if (nextSerial_ == std::numeric_limits<std::uint64_t>::max())
            throw ExecutionFailure("object allocation serial limit exceeded");
        auto candidate = domain_->version(std::make_unique<State>(std::move(initial)), this);
        domain_->ensureCapacity(candidate);
        // Reserve rollback bookkeeping before publishing a provisional cell.
        allocated_.reserve(allocated_.size() + 1);
        const auto id = domain_->allocate(Tag::typeId, std::move(candidate), this);
        allocated_.push_back({id, ++nextSerial_});
        try {
            auto result = domain_->template root<Tag>(id);
            prepared_ = false;
            return result;
        } catch (...) {
            markReachabilityChanged();
            domain_->destroy(id);
            allocated_.pop_back();
            throw;
        }
    }

    void validateInitialized() const;

    template<class Tag>
    void validateInitialized(const ObjectRef<Tag>& reference) const {
        validateVersion(*current(checkedReference(reference)));
    }

    void prepareCommit();

    template<class CommitRoot>
    void commit(CommitRoot commitRoot) {
        static_assert(std::is_nothrow_invocable_v<CommitRoot>);
        if (!prepared_ || hasSavepoint())
            std::terminate();
        domain_->requireOwner();
        requirePublication();
        publishUnchecked(std::move(commitRoot));
    }

private:
    template<class State>
    friend class GraphModelTransaction;

    void requirePublication() const;

    template<class CommitRoot>
    void publishUnchecked(CommitRoot commitRoot) noexcept {
        static_assert(std::is_nothrow_invocable_v<CommitRoot>);
        for (const auto& [id, value] : changes_) {
            auto& cell = domain_->cells_[id.slot];
            if (cell.state && cell.generation == id.generation)
                cell.state = value;
        }
        for (const auto& allocation : allocated_) {
            const auto id = allocation.id;
            auto& cell = domain_->cells_[id.slot];
            if (cell.state && cell.generation == id.generation)
                cell.provisional = nullptr;
        }
        commitRoot();
        // The precommit trace retained old and new edges; publication can drop
        // old reachability without destroying a RootLease. Sweep once more.
        if (!changes_.empty() || !allocated_.empty())
            domain_->rootSetDirty_.store(true, std::memory_order_release);
        changes_.clear();
        allocated_.clear();
        prepared_ = false;
    }

public:
    void commit();

    void discard() noexcept;

    void refresh();

    const std::shared_ptr<ObjectDomain>& domain() const noexcept;

private:
    friend class ObjectDomain;

    template<class State>
    static State& stateAs(ObjectState& state) {
        auto* typed = dynamic_cast<State*>(&state);
        if (!typed)
            throw ExecutionFailure("object state type mismatch");
        return *typed;
    }

    template<class State>
    static const State& stateAs(const ObjectState& state) {
        auto* typed = dynamic_cast<const State*>(&state);
        if (!typed)
            throw ExecutionFailure("object state type mismatch");
        return *typed;
    }

    bool isLive(ObjectId id) const noexcept;

    template<class Tag>
    ObjectId checkedReference(const ObjectRef<Tag>& reference) const {
        if (reference.ownerDomain() != domain_)
            throw ExecutionFailure("object belongs to a different owner domain");
        const auto id = reference.identity();
        const auto& cell = domain_->checked(id);
        if (cell.provisional && cell.provisional != this)
            throw ExecutionFailure("object construction is not committed");
        return id;
    }

    object_detail::VersionPtr current(ObjectId id) const;

    void validateVersion(const object_detail::Version& version) const;

    bool hasSavepoint() const noexcept;

    void markReachabilityChanged() noexcept;

    void rollbackAllocations(std::uint64_t serial) noexcept;

    std::shared_ptr<ObjectDomain> domain_;
    PublicationGuard publication_ = currentPublicationGuard();
    object_detail::Changes changes_;
    struct Allocation {
        ObjectId id;
        std::uint64_t serial;
    };

    std::vector<Allocation> allocated_;
    std::uint64_t nextSerial_ = 0;
    std::array<Savepoint*, maximumSavepoints> savepoints_{};
    bool prepared_ = false;
};

// Owned records without references use this fallback. Generated records with
// live references are refused in this slice; they cannot hide edges here.
template<class T>
struct ObjectStorage {
    using Stored = T;

    static Stored encode(const T& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return value;
    }

    static T decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return value;
    }

    static void trace(const Stored&, ObjectTrace&) {
    }
};

template<class Tag>
struct ObjectStorage<ObjectRef<Tag>> {
    using Stored = ObjectEdge<Tag>;

    static Stored encode(const ObjectRef<Tag>& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        if (value.isNull())
            return {};
        if (value.ownerDomain() != domain)
            throw ExecutionFailure("object belongs to a different owner domain");
        return {value.identity()};
    }

    static ObjectRef<Tag> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        if (value.id.isNull())
            return {};
        return domain->template root<Tag>(value.id);
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        trace.edge(value.id);
    }
};

template<class T>
struct ObjectStorage<Optional<T>> {
    using Element = ObjectStorage<T>;
    using Stored = Optional<typename Element::Stored>;

    static Stored encode(const Optional<T>& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return value.has_value() ? Stored::Some(Element::encode(value.value(), domain)) : Stored::None();
    }

    static Optional<T> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return value.has_value() ? Optional<T>::Some(Element::decode(value.value(), domain)) : Optional<T>::None();
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        if (value.has_value())
            Element::trace(value.value(), trace);
    }
};

namespace object_detail {

template<template<class> class Sequence, class T>
struct SequenceStorage {
    using Element = ObjectStorage<T>;
    using Stored = Sequence<typename Element::Stored>;

    static Stored encode(const Sequence<T>& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        Stored result;
        if constexpr (std::is_same_v<Sequence<T>, Array<T>>) {
            result = Stored::Create(value.count(), typename Element::Stored{});
            for (std::int64_t index = 0; index < value.count(); ++index)
                result.set(index, Element::encode(value.at(index), domain));
        } else if constexpr (std::is_same_v<Sequence<T>, Stack<T>>) {
            auto values = value.values();
            for (auto position = values.rbegin(); position != values.rend(); ++position)
                result.push(Element::encode(*position, domain));
        } else {
            for (const auto& item : value.values()) {
                if constexpr (std::is_same_v<Sequence<T>, Queue<T>>)
                    result.enqueue(Element::encode(item, domain));
                else
                    result.add(Element::encode(item, domain));
            }
        }
        return result;
    }

    // Read only the selected owned element. Stored object edges must never
    // escape; decoding restores the normal root lease for object references.
    static T storedAt(const Stored& value, std::int64_t index, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return Element::decode(value.at(index), domain);
    }

    static std::int64_t storedCount(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        return value.count();
    }

    static Sequence<T> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        Sequence<T> result;
        if constexpr (std::is_same_v<Sequence<T>, Array<T>>) {
            if (value.count() == 0)
                return result;
            result = Sequence<T>::Create(value.count(), Element::decode(value.at(0), domain));
            for (std::int64_t index = 0; index < value.count(); ++index)
                result.set(index, Element::decode(value.at(index), domain));
        } else if constexpr (std::is_same_v<Sequence<T>, Stack<T>>) {
            auto values = value.values();
            for (auto position = values.rbegin(); position != values.rend(); ++position)
                result.push(Element::decode(*position, domain));
        } else {
            for (const auto& item : value.values()) {
                if constexpr (std::is_same_v<Sequence<T>, Queue<T>>)
                    result.enqueue(Element::decode(item, domain));
                else
                    result.add(Element::decode(item, domain));
            }
        }
        return result;
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        for (const auto& item : value.values())
            Element::trace(item, trace);
    }
};

}

template<class T>
struct ObjectStorage<List<T>> : object_detail::SequenceStorage<List, T> {
};

template<class T>
struct ObjectStorage<Array<T>> : object_detail::SequenceStorage<Array, T> {
};

template<class T>
struct ObjectStorage<Queue<T>> : object_detail::SequenceStorage<Queue, T> {
};

template<class T>
struct ObjectStorage<Stack<T>> : object_detail::SequenceStorage<Stack, T> {
};

template<class K, class V>
struct ObjectStorage<Dictionary<K, V>> {
    using Element = ObjectStorage<V>;
    using Stored = Dictionary<K, typename Element::Stored>;

    static Stored encode(const Dictionary<K, V>& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        Stored result;
        for (const auto& item : value.values())
            result.add(item.key, Element::encode(item.value, domain));
        return result;
    }

    static Dictionary<K, V> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        Dictionary<K, V> result;
        for (const auto& item : value.values())
            result.add(item.key, Element::decode(item.value, domain));
        return result;
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        for (const auto& item : value.values())
            Element::trace(item.value, trace);
    }
};

template<class T, class E>
struct ObjectStorage<Result<T, E>> {
    using Value = ObjectStorage<T>;
    using ErrorStorage = ObjectStorage<E>;
    using Stored = Result<typename Value::Stored, typename ErrorStorage::Stored>;

    static Stored encode(const Result<T, E>& value, const std::shared_ptr<ObjectDomain>& domain) {
        if (value.is_success())
            return Stored::Success(Value::encode(value.value(), domain));
        auto error = ErrorStorage::encode(value.error(), domain);
        return value.is_cancelled() ? Stored::Cancelled(std::move(error)) : Stored::Failure(std::move(error));
    }

    static Result<T, E> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        if (value.is_success())
            return Result<T, E>::Success(Value::decode(value.value(), domain));
        auto error = ErrorStorage::decode(value.error(), domain);
        return value.is_cancelled() ? Result<T, E>::Cancelled(std::move(error)) : Result<T, E>::Failure(std::move(error));
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        if (value.is_success()) {
            Value::trace(value.value(), trace);
        } else {
            ErrorStorage::trace(value.error(), trace);
        }
    }
};

template<class E>
struct ObjectStorage<Result<void, E>> {
    using ErrorStorage = ObjectStorage<E>;
    using Stored = Result<void, typename ErrorStorage::Stored>;

    static Stored encode(const Result<void, E>& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        if (value.is_success())
            return Stored::Success();
        auto error = ErrorStorage::encode(value.error(), domain);
        return value.is_cancelled() ? Stored::Cancelled(std::move(error)) : Stored::Failure(std::move(error));
    }

    static Result<void, E> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        domain->requireOwner();
        if (value.is_success())
            return Result<void, E>::Success();
        auto error = ErrorStorage::decode(value.error(), domain);
        return value.is_cancelled() ? Result<void, E>::Cancelled(std::move(error)) : Result<void, E>::Failure(std::move(error));
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        if (!value.is_success())
            ErrorStorage::trace(value.error(), trace);
    }
};

template<class K, class V>
struct ObjectStorage<KeyValuePair<K, V>> {
    using Key = ObjectStorage<K>;
    using Value = ObjectStorage<V>;
    using Stored = KeyValuePair<typename Key::Stored, typename Value::Stored>;

    static Stored encode(const KeyValuePair<K, V>& value, const std::shared_ptr<ObjectDomain>& domain) {
        return {Key::encode(value.key, domain), Value::encode(value.value, domain)};
    }

    static KeyValuePair<K, V> decode(const Stored& value, const std::shared_ptr<ObjectDomain>& domain) {
        return {Key::decode(value.key, domain), Value::decode(value.value, domain)};
    }

    static void trace(const Stored& value, ObjectTrace& trace) {
        Key::trace(value.key, trace);
        Value::trace(value.value, trace);
    }
};

struct ObjectRootCallbacks {
    void* context = nullptr;
    void (*commit)(void*) = nullptr;
    void (*refresh)(void*) = nullptr;
    std::shared_ptr<void> lifetime;
};

class ObjectExecution {
public:
    ObjectExecution(TaskScope& scope, EvaluationBudget& budget, GraphTransaction& transaction,
                    std::shared_ptr<ObjectRootCallbacks> callbacks)
        : scope(scope), budget(budget), transaction(transaction), callbacks_(std::move(callbacks)) {
    }

    [[nodiscard]] ObjectExecution enterCall() const & {
        const auto entered = calls_.enter();
        ObjectExecution child(scope, budget, transaction, {});
        // A synchronous child cannot outlive its parent. Borrow the original
        // lease instead of incrementing a shared_ptr for every source call.
        child.borrowedCallbacks_ = &callbackOwner();
        child.borrowedCancellation_ = &cancellationToken();
        child.calls_ = entered;
        child.maximumDegree = maximumDegree;
        return child;
    }

    ObjectExecution enterCall() && = delete;
    ObjectExecution enterCall() const && = delete;

    [[nodiscard]] ObjectExecution enterAsyncCall() const {
        auto child = enterCall();
        // A lazy native coroutine can outlive the factory's stack context.
        // Retain the original callback owner in that frame, even when the
        // invoking synchronous context was itself borrowing the lease.
        child.callbacks_ = callbackOwner();
        child.borrowedCallbacks_ = nullptr;
        child.cancellation = cancellationToken();
        child.borrowedCancellation_ = nullptr;
        return child;
    }

    const CancellationToken& cancellationToken() const noexcept {
        return borrowedCancellation_ ? *borrowedCancellation_ : cancellation;
    }

    [[nodiscard]] ObjectExecution withCalls(SourceCallContext calls) const {
        auto child = *this;
        child.calls_ = calls;
        return child;
    }

    SourceCallContext calls() const noexcept {
        return calls_;
    }

    void commitSegment() {
        const auto& callbacks = callbackOwner();
        if (callbacks && callbacks->commit)
            callbacks->commit(callbacks->context);
        else
            transaction.commit();
    }

    void refreshSegment() {
        const auto& callbacks = callbackOwner();
        if (callbacks && callbacks->refresh)
            callbacks->refresh(callbacks->context);
        else
            transaction.refresh();
    }

    std::size_t maximumDegree = 4;
    CancellationToken cancellation;
    TaskScope& scope;
    EvaluationBudget& budget;
    GraphTransaction& transaction;

private:
    const std::shared_ptr<ObjectRootCallbacks>& callbackOwner() const noexcept {
        return borrowedCallbacks_ ? *borrowedCallbacks_ : callbacks_;
    }

    std::shared_ptr<ObjectRootCallbacks> callbacks_;
    const std::shared_ptr<ObjectRootCallbacks>* borrowedCallbacks_ = nullptr;
    const CancellationToken* borrowedCancellation_ = nullptr;
    SourceCallContext calls_;
};

template<class Tag>
void validate(const ObjectRef<Tag>& value) {
    value.requireValid();
}

}

// Header-only mode owns inline definitions; linked mode uses the SDK archive.
#if !defined(PIPELANG_MODEL_LINKED_RUNTIME) || !PIPELANG_MODEL_LINKED_RUNTIME
#define PIPELANG_MODEL_RUNTIME_INLINE inline
#include "detail/object_domain_impl.hpp"
#undef PIPELANG_MODEL_RUNTIME_INLINE
#endif
