// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeReceiverStorage.hpp"

#include "NativeReceiverJournal.hpp"
#include "native_app_execution.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace pipelang::native_app_v19 {
namespace {

class ReclamationGuard {
public:
    explicit ReclamationGuard(bool& active) : active_(active), previous_(std::exchange(active, true)) {}
    ~ReclamationGuard() { active_ = previous_; }

private:
    bool& active_;
    bool previous_;
};

}

model::TaskScope& NativeReceiverStorage::contextScope()
{
    model::context_detail::requireOwner(contextThread_);
    if (!contextScope_) {
        const model::InvocationContextScope parent(contextParent_);
        contextScope_.emplace();
    }
    return *contextScope_;
}

void NativeReceiverStorage::enableObjectReferences()
{
    exportedOwner_ = std::make_shared<const ExportedReceiverOwner>(ExportedReceiverOwner{contextThread_});
    referenceDomain_.exportedOwner = exportedOwner_.get();
}

std::shared_ptr<const ExportedReceiverReference> NativeReceiverStorage::exportReference(const void* receiver)
{
    model::context_detail::requireOwner(contextThread_);
    if (!exportedOwner_ || !graph_ || !referenceDomain_.owns(receiver))
        throw model::ExecutionFailure("cannot export an object outside its native hosted binding");

    const auto existing = exportedRoots_.find(receiver);
    if (existing != exportedRoots_.end()) {
        if (auto lease = existing->second.lease.lock())
            return lease;
    }

    auto lease = std::make_shared<const ExportedReceiverReference>(
        ExportedReceiverReference{exportedOwner_, receiver});
    if (existing != exportedRoots_.end()) {
        existing->second.lease = lease;
    } else {
        const auto allocation = receivers_.identity(receiver);
        exportedRoots_.emplace(receiver, ExportedRoot{lease, allocation});
        if (allocation != 0)
            pinReceiver(receiver, allocation);
    }
    return lease;
}

void NativeReceiverStorage::finishBinding()
{
    // Capture constructor-owned edges after the last savepoint has closed,
    // using the same receiver words that the binding will publish.
    for (auto& [receiver, fields] : constructedValues_) {
        for (auto& [field, value] : fields) {
            HostedProjection projection(constructorCollections, &referenceDomain_);
            (void)projection.copy(value.shape, field);
            value.references = projection.takeReferences();
        }
    }
    if (preciseRoots_)
        collectGraph(true);
    else if (graph_)
        constructorRoots_ = receivers_.checkpoint();
}

void NativeReceiverStorage::recordReferenceField(const void* receiver, const std::uint64_t* slot)
{
    references_[receiver].insert(slot);
    invalidateGraphMark();
}

void NativeReceiverStorage::recordSynchronizationField(const void* receiver, const std::uint64_t* field,
                                                       ReceiverSynchronizationKind kind)
{
    synchronizationFields_[receiver].try_emplace(field, kind);
    invalidateGraphMark();
}

void NativeReceiverStorage::recordConstructedValue(const void* receiver, const std::uint64_t* field,
                                                  const OwnedShape& shape)
{
    constructedValues_[receiver].try_emplace(field, ConstructedValue{shape, {}});
    invalidateGraphMark();
}

void NativeReceiverStorage::recordConstructionByteFields(const OwnedShape& shape, std::uint64_t* words)
{
    if (shape.kind == OwnedKind::ByteBuffer) {
        constructionByteFields_.insert(words);
    } else if (shape.kind == OwnedKind::Record) {
        for (const auto& field : std::span(shape.fields, shape.fieldCount))
            recordConstructionByteFields(*field.shape, words + field.offsetWords);
    }
}

void NativeReceiverStorage::retainConstructionByteFields(HostedInput& input)
{
    // Normalize each receiver field after construction. Its previous values
    // can share the constructor byte arena while construction is active.
    for (auto* field : constructionByteFields_) {
        ByteView value;
        std::memcpy(&value, field, sizeof(value));
        if (value.block == 0)
            continue;
        const auto retained = input.retainBytes(constructorCollections.bytes().view(value));
        std::memcpy(field, &retained, sizeof(retained));
    }
    constructionByteFields_.clear();
}

std::uint64_t NativeReceiverStorage::retainSynchronization(const void* receiver, ReceiverSynchronizationKind kind,
                                                           std::uint64_t handle)
{
    try {
        synchronizationCells_.try_emplace({kind, handle}, SynchronizationCell{receiver});
    } catch (...) {
        retireSynchronization(kind, handle);
        throw;
    }
    invalidateGraphMark();
    return hostedSynchronizationTag | handle;
}

void NativeReceiverStorage::discardHostedSynchronization(ReceiverSynchronizationKind kind,
                                                         std::uint64_t tagged) noexcept
{
    const auto handle = tagged & ~hostedSynchronizationTag;
    synchronizationCells_.erase({kind, handle});
    retireSynchronization(kind, handle);
}

void NativeReceiverStorage::restoreConstruction(std::size_t checkpoint) noexcept
{
    const ReclamationGuard reclaiming(reclaiming_);
    invalidateGraphMark();
    // Source synchronization fields are assigned only in the owning
    // constructor. Failed construction also rolls back receiver writes,
    // so new promoted handles cannot reach surviving receivers.
    for (auto cell = synchronizationCells_.begin(); cell != synchronizationCells_.end();) {
        if (!receivers_.allocatedSince(cell->second.receiver, checkpoint)) {
            ++cell;
            continue;
        }
        const auto [kind, handle] = cell->first;
        retireSynchronization(kind, handle);
        cell = synchronizationCells_.erase(cell);
    }
    receivers_.restore(checkpoint, [&](const void* receiver, std::size_t words) noexcept {
        discardReceiver(receiver, words);
    });
}

void NativeReceiverStorage::discardReceiver(const void* receiver, std::size_t words) noexcept
{
    const auto* begin = static_cast<const std::uint64_t*>(receiver);
    const auto* end = begin + words;
    text.discard(begin, end);
    sets.discard(begin, end);
    values.discard(receiver);
    references_.erase(receiver);
    exportedRoots_.erase(receiver);
    constructedValues_.erase(receiver);
    constructionByteFields_.erase(constructionByteFields_.lower_bound(begin),
                                  constructionByteFields_.lower_bound(end));
    synchronizationFields_.erase(receiver);
    defaultHandles_.erase(defaultHandles_.lower_bound(begin), defaultHandles_.lower_bound(end));
}

void NativeReceiverStorage::collectGraph(bool reclaim)
{
    // The host can drop an immutable reference value on any thread. Only
    // the receiver's owner thread updates graph pins and storage.
    for (auto root = exportedRoots_.begin(); root != exportedRoots_.end();) {
        if (!root->second.lease.expired()) {
            ++root;
            continue;
        }
        releaseReceiver(root->first, root->second.allocation);
        root = exportedRoots_.erase(root);
    }

    // Object allocations and ownership writes invalidate the last successful
    // sweep. Scalar writes and reads leave its reachability unchanged.
    if (!graph_ || !graphDirty_)
        return;
    const auto edges = [&](const void* receiver, const auto& visit) {
        const auto edge = [&](const void* reference) {
            if (reference != referenceDomain_.root)
                visit(reference);
        };
        values.visitReferences(receiver, edge);
        const auto constructed = constructedValues_.find(receiver);
        if (constructed != constructedValues_.end()) {
            for (const auto& [field, value] : constructed->second) {
                if (values.read(receiver, field))
                    continue;
                for (const auto* reference : value.references)
                    edge(reference);
            }
        }
        if (const auto found = references_.find(receiver); found != references_.end()) {
            for (const auto* slot : found->second)
                edge(reinterpret_cast<const void*>(*slot));
        }
    };
    const auto roots = [&](const auto& visit) {
        edges(referenceDomain_.root, visit);
    };
    const bool reuseFinalMark = std::exchange(finalGraphMarked_, false) && reclaim;
    if (!reuseFinalMark) {
        receivers_.markReachable(constructorRoots_, roots, edges);
        graphMarkValidated_ = true;
    }
    if (reclaim) {
        const ReclamationGuard reclaiming(reclaiming_);
        receivers_.reclaimUnreachable(
            [&](const void* receiver, std::size_t words) noexcept { discardReceiver(receiver, words); });
        collectSynchronization();
        graphDirty_ = false;
    }
}

void NativeReceiverStorage::retireSynchronization(ReceiverSynchronizationKind kind, std::uint64_t handle) noexcept
{
    switch (kind) {
    case ReceiverSynchronizationKind::Atomic: atomics.retire(handle); break;
    case ReceiverSynchronizationKind::Lock: locks.retire(handle); break;
    case ReceiverSynchronizationKind::Semaphore: semaphores.retire(handle); break;
    case ReceiverSynchronizationKind::ConcurrentSequence: sequences.retire(handle); break;
    case ReceiverSynchronizationKind::ConcurrentDictionary: dictionaries.retire(handle); break;
    case ReceiverSynchronizationKind::ContextOwner: contexts.retire(handle); break;
    }
}

void NativeReceiverStorage::collectSynchronization() noexcept
{
    const auto mark = [&](ReceiverSynchronizationKind kind, std::uint64_t tagged) {
        if ((tagged & hostedSynchronizationTag) == 0)
            return;
        auto cell = synchronizationCells_.find({kind, tagged & ~hostedSynchronizationTag});
        if (cell != synchronizationCells_.end())
            cell->second.marked = true;
    };
    for (const auto& [receiver, fields] : synchronizationFields_) {
        for (const auto& [field, kind] : fields) {
            mark(kind, *field);
            const auto fallback = defaultHandles_.find(field);
            if (fallback != defaultHandles_.end())
                mark(kind, fallback->second);
        }
    }
    for (auto cell = synchronizationCells_.begin(); cell != synchronizationCells_.end();) {
        if (cell->second.marked) {
            cell->second = {};
            ++cell;
            continue;
        }
        const auto [kind, handle] = cell->first;
        retireSynchronization(kind, handle);
        cell = synchronizationCells_.erase(cell);
    }
}


HostedValue projectReceiverSnapshot(const NativeReceiverStorage& storage, const std::uint64_t* receiver,
                                    const OwnedShape& shape)
{
    NativeLists lists;
    HostedProjection initial(storage.constructorCollections);
    std::vector<std::uint64_t> words(shape.words);
    std::vector<HostedProjection::RetainedField> retainedFields;
    const auto* current = receiver + receiverHeaderWords;
    const auto* journal = storage.snapshotWrites;
    const auto* committed = journal ? journal->committedFields(receiver) : current;
    for (const auto& field : std::span(shape.fields, shape.fieldCount)) {
        const auto& type = *field.shape;
        const auto* source = committed + field.offsetWords;
        const auto* address = current + field.offsetWords;
        auto* output = words.data() + field.offsetWords;
        std::copy_n(source, type.words, output);
        visitReceiverValues(type, 0, [&](const OwnedShape& leaf, std::size_t offset) {
            const auto* retained = storage.values.find(receiver, address + offset);
            const auto saved = retained
                ? (journal ? journal->committed(retained) : retained->current()) : NativeReceiverValues::Value{};
            if (saved) {
                retainedFields.push_back({output + offset, &leaf, saved->value, saved->bytes});
            } else {
                const auto before = initial.bytes();
                auto value = initial.copy(leaf, source + offset);
                retainedFields.push_back({output + offset, &leaf, std::move(value), initial.bytes() - before});
            }
        });
        visitReceiverSets(type, [&](const OwnedShape& key, std::size_t offset) {
            const auto* retained = storage.sets.find(address + offset);
            const auto saved = retained
                ? (journal ? journal->committed(retained) : retained->current()) : NativeReceiverSets::Value{};
            const auto members = saved.initialized ? saved.snapshot : storage.sets.readInitial(source + offset, key);
            const auto view = lists.sets().retain(members, key);
            std::memcpy(output + offset, &view, sizeof(view));
        });
    }
    std::sort(retainedFields.begin(), retainedFields.end(), [](const auto& left, const auto& right) {
        return std::less<>{}(left.address, right.address);
    });
    return HostedProjection(lists, nullptr, retainedFields).copy(shape, words.data());
}

}
