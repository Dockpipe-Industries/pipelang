// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
// Canonical ordinary graph ownership bodies. Included after object.hpp declarations.
// PIPELANG_MODEL_RUNTIME_INLINE selects header-only or SDK translation-unit ownership.

namespace pipelang::model {

PIPELANG_MODEL_RUNTIME_INLINE object_detail::RootLease::~RootLease() noexcept {
    if (domain)
        domain->rootSetDirty_.store(true, std::memory_order_release);
}

PIPELANG_MODEL_RUNTIME_INLINE ObjectDomain::ObjectDomain() : owner_(std::this_thread::get_id()) {
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::requireOwner() const {
    if (owner_ != std::this_thread::get_id())
        throw ExecutionFailure("object accessed from a different owner thread");
    if (closed_)
        throw ExecutionFailure("object domain is closed");
}

PIPELANG_MODEL_RUNTIME_INLINE std::uint64_t ObjectDomain::runtimeType(ObjectId id) const {
    return checked(id).typeId;
}

PIPELANG_MODEL_RUNTIME_INLINE ObjectDomain::Cell& ObjectDomain::checked(ObjectId id) {
    requireOwner();
    if (id.isNull() || id.slot >= maximumCells)
        throw ExecutionFailure("object reference is uninitialized");
    auto& cell = cells_[id.slot];
    if (!cell.state || cell.generation != id.generation)
        throw ExecutionFailure("object identity is no longer valid");
    return cell;
}

PIPELANG_MODEL_RUNTIME_INLINE const ObjectDomain::Cell& ObjectDomain::checked(ObjectId id) const {
    return const_cast<ObjectDomain*>(this)->checked(id);
}

PIPELANG_MODEL_RUNTIME_INLINE GraphTransaction::GraphTransaction(std::shared_ptr<ObjectDomain> domain)
    : domain_(std::move(domain)) {
    domain_->requireOwner();
    domain_->addTransaction(this);
}

PIPELANG_MODEL_RUNTIME_INLINE GraphTransaction::~GraphTransaction() {
    discard();
    domain_->removeTransaction(this);
}

PIPELANG_MODEL_RUNTIME_INLINE GraphTransaction::Savepoint::Savepoint(GraphTransaction& transaction)
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

PIPELANG_MODEL_RUNTIME_INLINE GraphTransaction::Savepoint::~Savepoint() {
    if (!saved_.empty() || (!committed_ && !transaction_.changes_.empty()))
        transaction_.markReachabilityChanged();
    if (!committed_) {
        transaction_.changes_.swap(saved_);
        transaction_.rollbackAllocations(allocated_);
    }
    for (auto& slot : transaction_.savepoints_) {
        if (slot == this)
            slot = nullptr;
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::Savepoint::commit() noexcept {
    committed_ = true;
}

PIPELANG_MODEL_RUNTIME_INLINE GraphTransaction::Savepoint GraphTransaction::savepoint() {
    domain_->requireOwner();
    return Savepoint(*this);
}

PIPELANG_MODEL_RUNTIME_INLINE std::uint64_t GraphTransaction::allocationMark() const {
    domain_->requireOwner();
    return nextSerial_;
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::validateInitialized() const {
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

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::prepareCommit() {
    domain_->requireOwner();
    requirePublication();
    if (hasSavepoint())
        throw ExecutionFailure("cannot commit during object construction");
    // Collection may allocate its bounded marking scratch, so it belongs
    // to preparation, never after root/graph publication has begun.
    if (!allocated_.empty() || !changes_.empty() ||
        domain_->rootSetDirty_.load(std::memory_order_acquire))
        domain_->collect();
    validateInitialized();
    requirePublication();
    prepared_ = true;
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::requirePublication() const {
    if (!publication_.is_current())
        throw StaleContextCancelled();
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::commit() {
    prepareCommit();
    commit([]() noexcept {});
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::discard() noexcept {
    if (!changes_.empty())
        markReachabilityChanged();
    changes_.clear();
    rollbackAllocations(0);
    prepared_ = false;
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::refresh() {
    domain_->requireOwner();
    if (hasSavepoint())
        throw ExecutionFailure("cannot refresh during object construction");
    discard();
}

PIPELANG_MODEL_RUNTIME_INLINE const std::shared_ptr<ObjectDomain>& GraphTransaction::domain() const noexcept {
    return domain_;
}

PIPELANG_MODEL_RUNTIME_INLINE bool GraphTransaction::isLive(ObjectId id) const noexcept {
    const auto& cell = domain_->cells_[id.slot];
    return cell.state && cell.generation == id.generation;
}

PIPELANG_MODEL_RUNTIME_INLINE object_detail::VersionPtr GraphTransaction::current(ObjectId id) const {
    domain_->checked(id);
    const auto changed = changes_.find(id);
    if (changed != changes_.end())
        return changed->second;
    return domain_->cells_[id.slot].state;
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::validateVersion(const object_detail::Version& version) const {
    version.state->validateInitialized();
    for (auto edge : version.edges)
        domain_->checked(edge);
}

PIPELANG_MODEL_RUNTIME_INLINE bool GraphTransaction::hasSavepoint() const noexcept {
    for (auto* savepoint : savepoints_) {
        if (savepoint)
            return true;
    }
    return false;
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::markReachabilityChanged() noexcept {
    domain_->rootSetDirty_.store(true, std::memory_order_release);
}

PIPELANG_MODEL_RUNTIME_INLINE void GraphTransaction::rollbackAllocations(std::uint64_t serial) noexcept {
    if (!allocated_.empty() && allocated_.back().serial > serial)
        markReachabilityChanged();
    while (!allocated_.empty() && allocated_.back().serial > serial) {
        domain_->destroy(allocated_.back().id);
        allocated_.pop_back();
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::addTransaction(GraphTransaction* transaction) {
    requireOwner();
    for (auto& slot : transactions_) {
        if (!slot) {
            slot = transaction;
            return;
        }
    }
    throw ExecutionFailure("object transaction capacity exceeded");
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::removeTransaction(GraphTransaction* transaction) noexcept {
    for (auto& slot : transactions_) {
        if (slot == transaction)
            slot = nullptr;
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::destroy(ObjectId id) noexcept {
    auto& cell = cells_[id.slot];
    if (cell.state && cell.generation == id.generation) {
        --liveCells_;
        cell.state.reset();
        cell.root.reset();
        cell.provisional = nullptr;
        cell.typeId = 0;
        if (cell.generation == std::numeric_limits<std::uint64_t>::max())
            std::terminate();
        ++cell.generation;
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::close() noexcept {
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

PIPELANG_MODEL_RUNTIME_INLINE std::size_t ObjectDomain::liveCount() const {
    requireOwner();
    std::size_t count = 0;
    for (const auto& cell : cells_) {
        if (cell.state)
            ++count;
    }
    return count;
}

PIPELANG_MODEL_RUNTIME_INLINE std::pair<std::size_t, std::size_t> ObjectDomain::resourceCost() const {
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

PIPELANG_MODEL_RUNTIME_INLINE std::size_t ObjectDomain::logicalBytes() const {
    return resourceCost().first;
}

PIPELANG_MODEL_RUNTIME_INLINE object_detail::VersionPtr ObjectDomain::version(std::unique_ptr<ObjectState> state,
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

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::ensureCapacity(const object_detail::VersionPtr& candidate) {
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

PIPELANG_MODEL_RUNTIME_INLINE ObjectId ObjectDomain::allocate(std::uint64_t typeId, object_detail::VersionPtr state,
                                       GraphTransaction* transaction) {
    requireOwner();
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (std::size_t index = 0; index < maximumCells; ++index) {
            auto& cell = cells_[index];
            if (!cell.state) {
                cell.typeId = typeId;
                cell.state = state;
                cell.provisional = transaction;
                ++liveCells_;
                return {static_cast<std::uint32_t>(index), cell.generation};
            }
        }
        collect(state->edges);
    }
    throw ExecutionFailure("object live cell limit exceeded");
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::collect() {
    collect({});
}

PIPELANG_MODEL_RUNTIME_INLINE void ObjectDomain::collect(const std::vector<ObjectId>& extraRoots) {
    requireOwner();
    // Clear before tracing so lease drops during sweep remain pending.
    rootSetDirty_.store(false, std::memory_order_release);
    if (!extraRoots.empty())
        rootSetDirty_.store(true, std::memory_order_release);
    try {
        if (liveCells_ == 0) {
            for (auto id : extraRoots) {
                if (!id.isNull())
                    checked(id);
            }
            return;
        }
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
    } catch (...) {
        rootSetDirty_.store(true, std::memory_order_release);
        throw;
    }
}

}
