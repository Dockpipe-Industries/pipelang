// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeAtomics.hpp"
#include "NativeConcurrentDictionaries.hpp"
#include "NativeConcurrentSequences.hpp"
#include "NativeLists.hpp"
#include "native_host_projection.hpp"
#include "NativeReceiverText.hpp"
#include "NativeReceiverValues.hpp"
#include "NativeSynchronizationStorage.hpp"

#include <pipelang/model/parallel.hpp>

#include <memory>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {

class ReceiverSegment;

class NativeReceiverSets {
public:
    struct Value {
        NativeSets::Snapshot snapshot;
        bool initialized = false;
    };

    class Field {
    public:
        const Value& current() const { return current_; }

    private:
        friend class NativeReceiverSets;
        Value current_;
    };

    explicit NativeReceiverSets(const NativeSets& constructorSets) : constructorSets_(constructorSets) {}

    Field& field(const std::uint64_t* address) { return fields_[address]; }

    const Field* find(const std::uint64_t* address) const
    {
        const auto field = fields_.find(address);
        return field == fields_.end() ? nullptr : &field->second;
    }

    NativeSets::Snapshot read(const std::uint64_t* address, const OwnedShape& key) const
    {
        if (const auto* field = find(address); field && field->current().initialized)
            return field->current().snapshot;
        return readInitial(address, key);
    }

    NativeSets::Snapshot readInitial(const std::uint64_t* address, const OwnedShape& key) const
    {
        SetView initial;
        std::memcpy(&initial, address, sizeof(initial));
        return constructorSets_.snapshot(initial, key);
    }

    void admitReplacement(std::size_t previousBytes, std::size_t newBytes) const
    {
        if (newBytes > maximumBytes - (bytes_ - previousBytes))
            throw model::EvaluationLimitFailure("native receiver HashSet storage limit exceeded");
    }

    void replace(Field& field, Value value) noexcept
    {
        bytes_ -= NativeSets::snapshotBytes(field.current_.snapshot);
        bytes_ += NativeSets::snapshotBytes(value.snapshot);
        field.current_ = std::move(value);
    }

    static constexpr auto maximumBytes = 32 * model::maximumListBytes;

    void discard(const std::uint64_t* begin, const std::uint64_t* end) noexcept
    {
        for (auto field = fields_.lower_bound(begin); field != fields_.end() && fields_.key_comp()(field->first, end);) {
            replace(field->second, {});
            field = fields_.erase(field);
        }
    }

private:
    const NativeSets& constructorSets_;
    std::map<const std::uint64_t*, Field> fields_;
    std::size_t bytes_ = 0;
};

struct NativeReceiverStorage {
    NativeReceiverStorage(const void* root, NativeReceivers& children, const NativeLists& collections,
                          bool graph, bool preciseRoots)
        : text(root, children), sets(collections.sets()), constructorCollections(collections),
          receivers_(children), referenceDomain_{root, children}, graph_(graph), preciseRoots_(preciseRoots),
          contextParent_(model::currentPermit()), contextThread_(std::this_thread::get_id()) {}

    NativeReceivers* graphReceivers() { return graph_ ? &receivers_ : nullptr; }
    NativeReceivers& constructionReceivers() { return receivers_; }
    const ReceiverReferenceDomain* referenceDomain() const { return graph_ ? &referenceDomain_ : nullptr; }
    bool hasPreciseRoots() const noexcept { return preciseRoots_; }

    void finishBinding();

    void recordReferenceField(const void* receiver, const std::uint64_t* slot);

    void recordSynchronizationField(const void* receiver, const std::uint64_t* field,
                                    ReceiverSynchronizationKind kind);

    void recordConstructedValue(const void* receiver, const std::uint64_t* field, const OwnedShape& shape);

    void recordConstructionByteFields(const OwnedShape& shape, std::uint64_t* words);
    void retainConstructionByteFields(HostedInput& input);

    std::uint64_t retainSynchronization(const void* receiver, ReceiverSynchronizationKind kind,
                                        std::uint64_t handle);
    void discardHostedSynchronization(ReceiverSynchronizationKind kind, std::uint64_t tagged) noexcept;

    void restoreConstruction(std::size_t checkpoint) noexcept;

    void collectGraph(bool reclaim);
    void preserveFinalGraphMark() noexcept { finalGraphMarked_ = graphDirty_ && graphMarkValidated_; }
    void invalidateGraphMark() noexcept
    {
        graphDirty_ = true;
        finalGraphMarked_ = false;
        graphMarkValidated_ = false;
    }

    void enableObjectReferences();
    std::shared_ptr<const ExportedReceiverReference> exportReference(const void* receiver);

    template<class Initialize>
    std::uint64_t defaultSynchronization(const void* receiver, const std::uint64_t* field,
                                         ReceiverSynchronizationKind kind, const Initialize& initialize)
    {
        // The shared cell's identity and effects survive receiver rollback.
        auto [entry, inserted] = defaultHandles_.try_emplace(field, 0);
        if (inserted) {
            try {
                if (graph_)
                    recordSynchronizationField(receiver, field, kind);
                entry->second = retainSynchronization(receiver, kind, initialize());
            } catch (...) {
                defaultHandles_.erase(entry);
                throw;
            }
        }
        return entry->second;
    }

    model::TaskScope& contextScope();
    void setParallelOptions(model::ParallelOptions options) noexcept { parallelOptions_ = options; }
    model::ParallelOptions parallelOptions() const noexcept { return parallelOptions_; }

    bool exposesSnapshot = false;
    bool permitsMultipleTasks = false;
    bool exposesObjectReferences() const noexcept { return static_cast<bool>(exportedOwner_); }
    bool blocksReentry() const noexcept { return snapshotWrites || reclaiming_; }
    const ReceiverSegment* snapshotWrites = nullptr;

    void pinReceiver(const void* receiver, std::uint64_t identity) noexcept
    {
        receivers_.pin(receiver, identity);
        invalidateGraphMark();
    }

    void releaseReceiver(const void* receiver, std::uint64_t identity) noexcept
    {
        if (receivers_.unpin(receiver, identity))
            invalidateGraphMark();
    }

    std::span<const void* const> initialReferences(const void* receiver, const std::uint64_t* field) const
    {
        const auto owner = constructedValues_.find(receiver);
        if (owner == constructedValues_.end())
            return {};
        const auto found = owner->second.find(field);
        if (found == owner->second.end())
            return {};
        return found->second.references;
    }

    NativeContextOwners contexts;
    InvocationAtomics atomics;
    InvocationLocks locks;
    InvocationSemaphores semaphores;
    NativeConcurrentSequences sequences;
    NativeConcurrentDictionaries dictionaries;
    NativeReceiverText text;
    NativeReceiverSets sets;
    NativeReceiverValues values;
    const NativeLists& constructorCollections;

private:
    void discardReceiver(const void* receiver, std::size_t words) noexcept;

    void retireSynchronization(ReceiverSynchronizationKind kind, std::uint64_t handle) noexcept;

    void collectSynchronization() noexcept;

    NativeReceivers& receivers_;
    ReceiverReferenceDomain referenceDomain_;
    bool graph_;
    bool preciseRoots_;
    bool graphDirty_ = true;
    bool finalGraphMarked_ = false;
    bool graphMarkValidated_ = false;
    bool reclaiming_ = false;
    std::shared_ptr<const ExportedReceiverOwner> exportedOwner_;
    struct ExportedRoot {
        std::weak_ptr<const ExportedReceiverReference> lease;
        std::uint64_t allocation;
    };
    std::map<const void*, ExportedRoot> exportedRoots_;
    model::InvocationPermit contextParent_;
    std::thread::id contextThread_;
    std::optional<model::TaskScope> contextScope_;
    // Earlier hosted graph descriptors lack constructor field metadata.
    std::size_t constructorRoots_ = 0;
    std::map<const void*, std::set<const std::uint64_t*>> references_;
    struct ConstructedValue {
        const OwnedShape& shape;
        std::vector<const void*> references;
    };
    std::map<const void*, std::map<const std::uint64_t*, ConstructedValue>> constructedValues_;
    std::set<std::uint64_t*, std::less<>> constructionByteFields_;
    std::map<const std::uint64_t*, std::uint64_t> defaultHandles_;
    std::map<const void*, std::map<const std::uint64_t*, ReceiverSynchronizationKind>> synchronizationFields_;
    struct SynchronizationCell {
        const void* receiver;
        bool marked = false;
    };

    std::map<std::pair<ReceiverSynchronizationKind, std::uint64_t>, SynchronizationCell> synchronizationCells_;
    model::ParallelOptions parallelOptions_;
};

template<class Visitor>
void visitReceiverSets(const OwnedShape& shape, const Visitor& visit)
{
    if (shape.kind == OwnedKind::HashSet) {
        visit(*shape.fields[0].shape, 0);
        return;
    }
    if (shape.kind != OwnedKind::Record)
        return;
    struct Field {
        const OwnedShape* shape;
        std::size_t offset;
    };
    std::vector<Field> remaining{{&shape, 0}};
    while (!remaining.empty()) {
        const auto field = remaining.back();
        remaining.pop_back();
        for (const auto& child : std::span(field.shape->fields, field.shape->fieldCount)) {
            const auto offset = field.offset + child.offsetWords;
            if (child.shape->kind == OwnedKind::Record)
                remaining.push_back({child.shape, offset});
            else if (child.shape->kind == OwnedKind::HashSet)
                visit(*child.shape->fields[0].shape, offset);
        }
    }
}

inline bool fixedArrayHasStaticOwnership(const OwnedShape& shape)
{
    switch (shape.kind) {
    case OwnedKind::Integer:
    case OwnedKind::UInt32:
    case OwnedKind::Boolean:
    case OwnedKind::Enum:
    case OwnedKind::Float:
    case OwnedKind::Float32:
        return true;
    case OwnedKind::FixedArray:
        return shape.valueCount == 0 || fixedArrayHasStaticOwnership(*shape.fields[0].shape);
    default:
        return false;
    }
}

template<class Visitor>
void visitReceiverValues(const OwnedShape& shape, std::size_t offset, const Visitor& visit)
{
    switch (shape.kind) {
    case OwnedKind::Record:
        // Assigning a nested member and replacing its enclosing record must
        // address the same retained value.
        for (const auto& child : std::span(shape.fields, shape.fieldCount))
            visitReceiverValues(*child.shape, offset + child.offsetWords, visit);
        return;
    case OwnedKind::FixedArray:
        if (!fixedArrayHasStaticOwnership(shape))
            visit(shape, offset);
        return;
    case OwnedKind::Array:
    case OwnedKind::List:
    case OwnedKind::Dictionary:
    case OwnedKind::Queue:
    case OwnedKind::Stack:
    case OwnedKind::Optional:
    case OwnedKind::Result:
    case OwnedKind::ResultInterface:
    case OwnedKind::Pair:
    case OwnedKind::Error:
        visit(shape, offset);
        return;
    default:
        return;
    }
}

}
