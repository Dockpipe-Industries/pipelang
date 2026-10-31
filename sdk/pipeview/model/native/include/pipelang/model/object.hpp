// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "extended_collections.hpp"
#include "task.hpp"

#include <array>
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

    ObjectDomain() : owner_(std::this_thread::get_id()) {
    }

    ObjectDomain(const ObjectDomain&) = delete;
    ObjectDomain& operator=(const ObjectDomain&) = delete;

    void requireOwner() const {
        if (owner_ != std::this_thread::get_id())
            throw ExecutionFailure("object accessed from a different owner thread");
        if (closed_)
            throw ExecutionFailure("object domain is closed");
    }

    void close() noexcept;
    void collect();
    std::size_t liveCount() const;
    std::size_t logicalBytes() const;

    std::uint64_t runtimeType(ObjectId id) const {
        return checked(id).typeId;
    }

    template<class Tag>
    ObjectRef<Tag> root(ObjectId id);

private:
    friend class GraphTransaction;

    struct Cell {
        std::uint64_t generation = 1;
        std::uint64_t typeId = 0;
        object_detail::VersionPtr state;
        GraphTransaction* provisional = nullptr;
        std::weak_ptr<object_detail::RootLease> root;
    };

    Cell& checked(ObjectId id) {
        requireOwner();
        if (id.isNull() || id.slot >= maximumCells)
            throw ExecutionFailure("object reference is uninitialized");
        auto& cell = cells_[id.slot];
        if (!cell.state || cell.generation != id.generation)
            throw ExecutionFailure("object identity is no longer valid");
        return cell;
    }

    const Cell& checked(ObjectId id) const {
        return const_cast<ObjectDomain*>(this)->checked(id);
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
    bool closed_ = false;
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
        // root() performs the identity and runtime tag checks without allocating
        // another lease: this handle itself keeps the cached lease alive.
        root_->domain->template root<Tag>(root_->id);
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
    auto& cell = checked(id);
    if (!Tag::accepts(cell.typeId))
        throw ExecutionFailure("object does not implement the requested type");
    auto lease = cell.root.lock();
    if (!lease) {
        lease = std::make_shared<object_detail::RootLease>();
        lease->domain = shared_from_this();
        lease->id = id;
        cell.root = lease;
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

    explicit GraphTransaction(std::shared_ptr<ObjectDomain> domain) : domain_(std::move(domain)) {
        domain_->requireOwner();
        domain_->addTransaction(this);
    }

    GraphTransaction(const GraphTransaction&) = delete;
    GraphTransaction& operator=(const GraphTransaction&) = delete;

    ~GraphTransaction() {
        discard();
        domain_->removeTransaction(this);
    }

    class Savepoint {
    public:
        explicit Savepoint(GraphTransaction& transaction)
            : transaction_(transaction), saved_(transaction.changes_),
              allocated_(transaction.nextSerial_) {
            for (auto& slot : transaction_.savepoints_) {
                if (!slot) {
                    slot = this;
                    return;
                }
            }
            throw ExecutionFailure("object construction nesting limit exceeded");
        }

        Savepoint(const Savepoint&) = delete;
        Savepoint& operator=(const Savepoint&) = delete;

        ~Savepoint() {
            if (!committed_) {
                transaction_.changes_.swap(saved_);
                transaction_.rollbackAllocations(allocated_);
            }
            for (auto& slot : transaction_.savepoints_) {
                if (slot == this)
                    slot = nullptr;
            }
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        friend class ObjectDomain;
        friend class GraphTransaction;
        GraphTransaction& transaction_;
        object_detail::Changes saved_;
        std::uint64_t allocated_;
        bool committed_ = false;
    };

    Savepoint savepoint() {
        domain_->requireOwner();
        return Savepoint(*this);
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
            domain_->destroy(id);
            allocated_.pop_back();
            throw;
        }
    }

    void validateInitialized() const {
        domain_->requireOwner();
        for (const auto& allocation : allocated_) {
            if (isLive(allocation.id))
                validateVersion(*current(allocation.id));
        }
        for (const auto& [id, value] : changes_) {
            if (isLive(id))
                validateVersion(*value);
        }
    }

    template<class Tag>
    void validateInitialized(const ObjectRef<Tag>& reference) const {
        validateVersion(*current(checkedReference(reference)));
    }

    void prepareCommit() {
        if (hasSavepoint())
            throw ExecutionFailure("cannot commit during object construction");
        // Collection may allocate its bounded marking scratch, so it belongs
        // to preparation, never after root/graph publication has begun.
        domain_->collect();
        validateInitialized();
        prepared_ = true;
    }

    template<class CommitRoot>
    void commit(CommitRoot commitRoot) noexcept {
        static_assert(std::is_nothrow_invocable_v<CommitRoot>);
        if (!prepared_ || hasSavepoint())
            std::terminate();
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
        changes_.clear();
        allocated_.clear();
        prepared_ = false;
    }

    void commit() {
        prepareCommit();
        commit([]() noexcept {});
    }

    void discard() noexcept {
        changes_.clear();
        rollbackAllocations(0);
        prepared_ = false;
    }

    void refresh() {
        domain_->requireOwner();
        if (hasSavepoint())
            throw ExecutionFailure("cannot refresh during object construction");
        discard();
    }

    const std::shared_ptr<ObjectDomain>& domain() const noexcept {
        return domain_;
    }

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

    bool isLive(ObjectId id) const noexcept {
        const auto& cell = domain_->cells_[id.slot];
        return cell.state && cell.generation == id.generation;
    }

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

    object_detail::VersionPtr current(ObjectId id) const {
        domain_->checked(id);
        const auto changed = changes_.find(id);
        if (changed != changes_.end())
            return changed->second;
        return domain_->cells_[id.slot].state;
    }

    void validateVersion(const object_detail::Version& version) const {
        version.state->validateInitialized();
        for (auto edge : version.edges)
            domain_->checked(edge);
    }

    bool hasSavepoint() const noexcept {
        for (auto* savepoint : savepoints_) {
            if (savepoint)
                return true;
        }
        return false;
    }

    void rollbackAllocations(std::uint64_t serial) noexcept {
        while (!allocated_.empty() && allocated_.back().serial > serial) {
            domain_->destroy(allocated_.back().id);
            allocated_.pop_back();
        }
    }

    std::shared_ptr<ObjectDomain> domain_;
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

inline void ObjectDomain::addTransaction(GraphTransaction* transaction) {
    requireOwner();
    for (auto& slot : transactions_) {
        if (!slot) {
            slot = transaction;
            return;
        }
    }
    throw ExecutionFailure("object transaction capacity exceeded");
}

inline void ObjectDomain::removeTransaction(GraphTransaction* transaction) noexcept {
    for (auto& slot : transactions_) {
        if (slot == transaction)
            slot = nullptr;
    }
}

inline void ObjectDomain::destroy(ObjectId id) noexcept {
    auto& cell = cells_[id.slot];
    if (cell.state && cell.generation == id.generation) {
        cell.state.reset();
        cell.root.reset();
        cell.provisional = nullptr;
        cell.typeId = 0;
        if (cell.generation == std::numeric_limits<std::uint64_t>::max())
            std::terminate();
        ++cell.generation;
    }
}

inline void ObjectDomain::close() noexcept {
    if (owner_ != std::this_thread::get_id())
        std::terminate();
    closed_ = true;
    for (auto* transaction : transactions_) {
        if (transaction) {
            transaction->discard();
            for (auto* savepoint : transaction->savepoints_) {
                if (savepoint)
                    savepoint->saved_.clear();
            }
        }
    }
    for (std::size_t index = 0; index < maximumCells; ++index)
        destroy({static_cast<std::uint32_t>(index), cells_[index].generation});
}

inline std::size_t ObjectDomain::liveCount() const {
    requireOwner();
    std::size_t count = 0;
    for (const auto& cell : cells_) {
        if (cell.state)
            ++count;
    }
    return count;
}

inline std::pair<std::size_t, std::size_t> ObjectDomain::resourceCost() const {
    requireOwner();
    // Count every distinct immutable version, including rollback savepoints.
    std::map<const object_detail::Version*, const object_detail::Version*> versions;
    std::size_t bytes = 0;
    std::size_t edges = 0;
    const auto add = [&](const object_detail::VersionPtr& version) {
        if (version)
            versions.emplace(version.get(), version.get());
    };
    // Published/provisional base versions are unique to one cell. Only overlay
    // and savepoint versions need deduplication, avoiding allocation per live
    // cell during the common creation/read-only path.
    for (const auto& cell : cells_) {
        if (cell.state) {
            bytes = boundedOwnedSum(bytes, cell.state->bytes, maximumBytes);
            edges = boundedOwnedSum(edges, cell.state->edges.size(), maximumEdges);
        }
    }
    for (auto* transaction : transactions_) {
        if (!transaction)
            continue;
        for (const auto& [id, version] : transaction->changes_)
            add(version);
        for (auto* savepoint : transaction->savepoints_) {
            if (savepoint) {
                for (const auto& [id, version] : savepoint->saved_)
                    add(version);
            }
        }
    }
    for (const auto& [identity, version] : versions) {
        bytes = boundedOwnedSum(bytes, version->bytes, maximumBytes);
        edges = boundedOwnedSum(edges, version->edges.size(), maximumEdges);
    }
    return {bytes, edges};
}

inline std::size_t ObjectDomain::logicalBytes() const {
    return resourceCost().first;
}

inline object_detail::VersionPtr ObjectDomain::version(std::unique_ptr<ObjectState> state,
                                                       GraphTransaction* transaction) {
    requireOwner();
    state->validate();
    auto version = std::make_shared<object_detail::Version>();
    version->bytes = state->logicalBytes();
    if (version->bytes > maximumBytes)
        throw ExecutionFailure("object logical byte limit exceeded");
    ObjectTrace trace;
    state->trace(trace);
    version->edges = trace.edges();
    version->state = std::move(state);
    for (auto edge : version->edges) {
        if (!edge.isNull()) {
            const auto& target = checked(edge);
            if (target.provisional && target.provisional != transaction)
                throw ExecutionFailure("object construction belongs to another transaction");
        }
    }
    return version;
}

inline void ObjectDomain::ensureCapacity(const object_detail::VersionPtr& candidate) {
    requireOwner();
    const auto fits = [&] {
        const auto [bytes, edges] = resourceCost();
        return candidate->bytes <= maximumBytes - std::min(bytes, maximumBytes)
            && candidate->edges.size() <= maximumEdges - std::min(edges, maximumEdges);
    };
    if (!fits()) {
        collect(candidate->edges);
        if (!fits())
            throw ExecutionFailure("object aggregate logical byte or edge limit exceeded");
    }
}

inline ObjectId ObjectDomain::allocate(std::uint64_t typeId, object_detail::VersionPtr state,
                                       GraphTransaction* transaction) {
    requireOwner();
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (std::size_t index = 0; index < maximumCells; ++index) {
            auto& cell = cells_[index];
            if (!cell.state) {
                cell.typeId = typeId;
                cell.state = state;
                cell.provisional = transaction;
                return {static_cast<std::uint32_t>(index), cell.generation};
            }
        }
        collect(state->edges);
    }
    throw ExecutionFailure("object live cell limit exceeded");
}

inline void ObjectDomain::collect() {
    collect({});
}

inline void ObjectDomain::collect(const std::vector<ObjectId>& extraRoots) {
    requireOwner();
    std::array<bool, maximumCells> marked{};
    std::vector<ObjectId> pending;
    pending.reserve(maximumCells);
    const auto mark = [&](ObjectId id) {
        if (id.isNull())
            return;
        checked(id);
        if (!marked[id.slot]) {
            marked[id.slot] = true;
            pending.push_back(id);
        }
    };
    for (std::size_t index = 0; index < maximumCells; ++index) {
        const auto& cell = cells_[index];
        if (cell.state && !cell.root.expired())
            mark({static_cast<std::uint32_t>(index), cell.generation});
    }
    for (auto id : extraRoots)
        mark(id);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        for (auto edge : cells_[id.slot].state->edges)
            mark(edge);
        for (auto* transaction : transactions_) {
            if (!transaction)
                continue;
            const auto visit = [&](const object_detail::Changes& changes) {
                const auto position = changes.find(id);
                if (position != changes.end()) {
                    for (auto edge : position->second->edges)
                        mark(edge);
                }
            };
            visit(transaction->changes_);
            for (auto* savepoint : transaction->savepoints_) {
                if (savepoint)
                    visit(savepoint->saved_);
            }
        }
    }
    for (std::size_t index = 0; index < maximumCells; ++index) {
        auto& cell = cells_[index];
        if (!cell.state || marked[index])
            continue;
        const ObjectId id{static_cast<std::uint32_t>(index), cell.generation};
        for (auto* transaction : transactions_) {
            if (transaction) {
                transaction->changes_.erase(id);
                for (auto* savepoint : transaction->savepoints_) {
                    if (savepoint)
                        savepoint->saved_.erase(id);
                }
            }
        }
        destroy(id);
    }
    for (auto* transaction : transactions_) {
        if (transaction) {
            auto& allocations = transaction->allocated_;
            allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
                [&](const GraphTransaction::Allocation& allocation) {
                    return !transaction->isLive(allocation.id);
                }), allocations.end());
        }
    }
}

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

    void commitSegment() {
        if (callbacks_ && callbacks_->commit)
            callbacks_->commit(callbacks_->context);
        else
            transaction.commit();
    }

    void refreshSegment() {
        if (callbacks_ && callbacks_->refresh)
            callbacks_->refresh(callbacks_->context);
        else
            transaction.refresh();
    }

    std::size_t maximumDegree = 4;
    CancellationToken cancellation;
    TaskScope& scope;
    EvaluationBudget& budget;
    GraphTransaction& transaction;

private:
    std::shared_ptr<ObjectRootCallbacks> callbacks_;
};

template<class Tag>
void validate(const ObjectRef<Tag>& value) {
    value.requireValid();
}

}
