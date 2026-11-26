// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeReceiverStorage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {

class ReceiverSegment {
public:
    explicit ReceiverSegment(std::size_t& aggregateWords) : aggregateWords_(aggregateWords) {}
    ReceiverSegment(const ReceiverSegment&) = delete;
    ReceiverSegment& operator=(const ReceiverSegment&) = delete;

    ~ReceiverSegment()
    {
        for (auto entry = others_.rbegin(); entry != others_.rend(); ++entry)
            restore(*entry);
        if (first_.receiver)
            restore(first_);
        for (auto& [field, previous] : textCheckpoints_)
            textOwner_->replace(*field, std::move(previous));
        for (auto& [field, previous] : setCheckpoints_)
            setOwner_->replace(*field, std::move(previous));
        for (auto& [field, previous] : valueCheckpoints_)
            valueOwner_->replace(*field, std::move(previous));
        aggregateWords_ -= usedWords_;
    }

    void track(void* receiver)
    {
        if (first_.receiver == receiver && receiver)
            return;
        if (!receiver)
            throw model::ExecutionFailure("object reference is uninitialized");
        const auto tracked = std::find_if(others_.begin(), others_.end(), [receiver](const auto& entry) {
            return entry.receiver == receiver;
        });
        if (tracked != others_.end())
            return;

        auto* storage = static_cast<std::uint64_t*>(receiver);
        const auto words = storage[receiverSizeWord];
        if (words == 0 || words > maximumReceiverWords || words > NativeReceivers::maximumWords - aggregateWords_)
            throw model::ExecutionFailure("native receiver checkpoint exceeds the segment storage limit");

        const auto* fields = storage + receiverHeaderWords;
        Checkpoint entry{storage, words, overflow_.size(), !first_.receiver && words <= small_.size()};
        if (entry.inlined) {
            std::copy_n(fields, words, small_.begin());
        } else {
            overflow_.insert(overflow_.end(), fields, fields + words);
        }
        if (!first_.receiver) {
            first_ = entry;
        } else {
            try {
                others_.push_back(entry);
            } catch (...) {
                overflow_.resize(entry.offset);
                throw;
            }
        }
        usedWords_ += words;
        aggregateWords_ += words;
    }

    const std::uint64_t* committedFields(const std::uint64_t* receiver) const
    {
        if (first_.receiver == receiver)
            return previousWords(first_);
        const auto entry = std::find_if(others_.begin(), others_.end(), [receiver](const auto& checkpoint) {
            return checkpoint.receiver == receiver;
        });
        return entry == others_.end() ? receiver + receiverHeaderWords : previousWords(*entry);
    }

    NativeReceiverSets::Value committed(const NativeReceiverSets::Field* field) const
    {
        const auto previous = setCheckpoints_.find(field);
        return previous == setCheckpoints_.end() ? field->current() : previous->second;
    }

    NativeReceiverValues::Value committed(const NativeReceiverValues::Field* field) const
    {
        const auto previous = valueCheckpoints_.find(field);
        return previous == valueCheckpoints_.end() ? field->current() : previous->second;
    }

    void commit()
    {
        first_ = {};
        others_.clear();
        overflow_.clear();
        aggregateWords_ -= std::exchange(usedWords_, 0);
        textCheckpoints_.clear();
        textOwner_ = nullptr;
        setCheckpoints_.clear();
        setOwner_ = nullptr;
        valueCheckpoints_.clear();
        valueOwner_ = nullptr;
    }

    void trackText(NativeReceiverText& owner, NativeReceiverText::Field& field)
    {
        textCheckpoints_.try_emplace(&field, field.current());
        textOwner_ = &owner;
    }

    void trackSet(NativeReceiverSets& owner, NativeReceiverSets::Field& field)
    {
        setCheckpoints_.try_emplace(&field, field.current());
        setOwner_ = &owner;
    }

    void trackValue(NativeReceiverValues& owner, NativeReceiverValues::Field& field)
    {
        valueCheckpoints_.try_emplace(&field, field.current());
        valueOwner_ = &owner;
    }

private:
    struct Checkpoint {
        std::uint64_t* receiver;
        std::size_t words;
        std::size_t offset;
        bool inlined;
    };

    const std::uint64_t* previousWords(const Checkpoint& checkpoint) const
    {
        return checkpoint.inlined ? small_.data() : overflow_.data() + checkpoint.offset;
    }

    void restore(const Checkpoint& checkpoint)
    {
        std::copy_n(previousWords(checkpoint), checkpoint.words, checkpoint.receiver + receiverHeaderWords);
    }

    Checkpoint first_{};
    std::array<std::uint64_t, 16> small_;
    std::vector<Checkpoint> others_;
    std::vector<std::uint64_t> overflow_;
    std::size_t& aggregateWords_;
    std::size_t usedWords_ = 0;
    NativeReceiverText* textOwner_ = nullptr;
    std::map<NativeReceiverText::Field*, NativeReceiverText::Value> textCheckpoints_;
    NativeReceiverSets* setOwner_ = nullptr;
    std::map<NativeReceiverSets::Field*, NativeReceiverSets::Value, std::less<>> setCheckpoints_;
    NativeReceiverValues* valueOwner_ = nullptr;
    std::map<NativeReceiverValues::Field*, NativeReceiverValues::Value, std::less<>> valueCheckpoints_;
};

class ReceiverTransactions {
public:
    struct ConstructionExit {
        std::size_t semaphoreCheckpoint;
        std::size_t atomicCheckpoint;
        std::size_t lockCheckpoint;
        std::size_t concurrentSequenceCheckpoint;
        std::size_t concurrentDictionaryCheckpoint;
        std::size_t contextCheckpoint;
        bool tracksDepth;
        std::size_t hostedSynchronizationCheckpoint = 0;
        std::uint64_t receiverValueCheckpoint = 0;
    };

    explicit ReceiverTransactions(NativeReceivers& receivers, NativeReceiverStorage* graph)
        : receivers_(&receivers), graph_(graph), segment_(aggregateWords_) {}

    explicit ReceiverTransactions(NativeReceiverStorage& binding)
        : receivers_(nullptr), graph_(&binding), segment_(aggregateWords_) {}

    ~ReceiverTransactions()
    {
        while (!constructors_.empty())
            finishConstruction(false);
    }

    void beginConstruction(NativeLists& values, ConstructionExit exit)
    {
        const auto graphCheckpoint = graph_ ? graph_->constructionReceivers().checkpoint() : 0;
        const auto allocation = receivers_ ? receivers_->checkpoint() : 0;
        constructors_.push_back(
            std::make_unique<Construction>(allocation, graphCheckpoint, aggregateWords_, values, exit));
    }

    ConstructionExit finishConstruction(bool succeeded) noexcept
    {
        const auto& transaction = constructors_.back();
        const auto allocation = transaction->allocation;
        const auto graphAllocation = transaction->graphAllocation;
        auto& values = transaction->values;
        const auto valueAllocation = transaction->valueAllocation;
        const auto exit = transaction->exit;
        if (succeeded)
            transaction->writes.commit();
        constructors_.pop_back();
        if (!succeeded) {
            if (receivers_)
                receivers_->restore(allocation);
            if (graph_)
                graph_->restoreConstruction(graphAllocation);
            values.restoreConstruction(valueAllocation);
        }
        return exit;
    }

    void track(void* receiver)
    {
        trackExisting(receiver, [&](ReceiverSegment& writes) { writes.track(receiver); });
    }

    void trackText(const void* receiver, NativeReceiverText& owner, NativeReceiverText::Field& field)
    {
        trackExisting(receiver, [&](ReceiverSegment& writes) { writes.trackText(owner, field); });
    }

    void trackSet(const void* receiver, NativeReceiverSets& owner, NativeReceiverSets::Field& field)
    {
        trackExisting(receiver, [&](ReceiverSegment& writes) { writes.trackSet(owner, field); });
    }

    void trackValue(const void* receiver, NativeReceiverValues& owner, NativeReceiverValues::Field& field)
    {
        trackExisting(receiver, [&](ReceiverSegment& writes) { writes.trackValue(owner, field); });
    }

    void commit() { segment_.commit(); }

    bool constructing() const noexcept { return !constructors_.empty(); }

    std::size_t protectedByteBlocks() const noexcept
    {
        return constructors_.empty() ? 0 : constructors_.back()->valueAllocation.byteBlocks;
    }

    std::size_t protectedListBlocks() const noexcept
    {
        return constructors_.empty() ? 0 : constructors_.back()->valueAllocation.listBlocks;
    }

    const ReceiverSegment& writes() const noexcept { return segment_; }

private:
    template<class Track>
    void trackExisting(const void* receiver, const Track& track)
    {
        // A failed construction discards its new receivers and their snapshots.
        // A journal can only refer to a receiver that survives its rollback.
        const auto allocatedIn = [&](const auto& transaction) {
            return (receivers_ && receivers_->allocatedSince(receiver, transaction.allocation)) ||
                (graph_ && graph_->constructionReceivers().allocatedSince(receiver, transaction.graphAllocation));
        };
        if (receivers_ && (constructors_.empty() || !allocatedIn(*constructors_.front())))
            track(segment_);
        for (auto& constructor : constructors_) {
            if (!allocatedIn(*constructor))
                track(constructor->writes);
        }
    }

    struct Construction {
        Construction(std::size_t allocation, std::size_t graphAllocation, std::size_t& aggregateWords,
                     NativeLists& values, ConstructionExit exit)
            : allocation(allocation), graphAllocation(graphAllocation), values(values),
              valueAllocation(values.constructionCheckpoint()), writes(aggregateWords), exit(exit) {}

        std::size_t allocation;
        std::size_t graphAllocation;
        NativeLists& values;
        NativeLists::ConstructionCheckpoint valueAllocation;
        ReceiverSegment writes;
        ConstructionExit exit;
    };

    NativeReceivers* receivers_;
    NativeReceiverStorage* graph_;
    std::size_t aggregateWords_ = 0;
    ReceiverSegment segment_;
    std::vector<std::unique_ptr<Construction>> constructors_;
};

}
