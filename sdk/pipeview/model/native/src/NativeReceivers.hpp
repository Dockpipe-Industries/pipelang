// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/object.hpp>

#include <array>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeReceivers {
public:
    static constexpr std::size_t maximumWords = model::ObjectDomain::maximumBytes / sizeof(std::uint64_t);

    bool owns(const void* receiver) const
    {
        return objects_.contains(receiver);
    }

    std::uint64_t identity(const void* receiver) const noexcept
    {
        const auto found = objects_.find(receiver);
        return found == objects_.end() ? 0 : found->second.identity;
    }

    void pin(const void* receiver, std::uint64_t expected) noexcept
    {
        const auto found = objects_.find(receiver);
        if (found != objects_.end() && found->second.identity == expected) {
            ++found->second.pins;
            ++activePins_;
        }
    }

    bool unpin(const void* receiver, std::uint64_t expected) noexcept
    {
        const auto found = objects_.find(receiver);
        if (found == objects_.end() || found->second.identity != expected || found->second.pins == 0)
            return false;
        --activePins_;
        return --found->second.pins == 0;
    }

    bool canShareAllocation(const NativeReceivers& other, std::uint64_t words) const noexcept
    {
        return words <= maximumReceiverWords &&
            objects_.size() + other.objects_.size() < model::ObjectDomain::maximumCells &&
            usedWords_ + other.usedWords_ + words + receiverHeaderWords <= maximumWords;
    }

    std::size_t checkpoint() const noexcept
    {
        return allocationOrder_.size();
    }

    bool allocatedSince(const void* receiver, std::size_t checkpoint) const
    {
        const auto allocation = objects_.find(receiver);
        return allocation != objects_.end() && allocation->second.index >= checkpoint;
    }

    template<class Roots, class Edges>
    void markReachable(std::size_t retainedPrefix, const Roots& roots, const Edges& edges)
    {
        for (auto& [address, allocation] : objects_)
            allocation.marked = false;

        std::array<const void*, model::ObjectDomain::maximumCells> pending;
        std::size_t count = 0;
        const auto visit = [&](const void* reference) {
            if (!reference)
                return;
            const auto found = objects_.find(reference);
            if (found == objects_.end())
                throw model::ExecutionFailure("native receiver reference does not belong to its graph");
            if (!found->second.marked) {
                found->second.marked = true;
                pending[count++] = reference;
            }
        };

        std::size_t edgeCount = 0;
        const auto edge = [&](const void* reference) {
            if (!reference)
                return;
            if (edgeCount == model::ObjectDomain::maximumEdges)
                throw model::EvaluationLimitFailure("native receiver graph edge limit exceeded");
            ++edgeCount;
            visit(reference);
        };
        for (std::size_t index = 0; index < retainedPrefix; ++index)
            visit(allocationOrder_[index]);
        if (activePins_ != 0) {
            for (const auto& [address, allocation] : objects_) {
                if (allocation.pins != 0)
                    visit(address);
            }
        }
        roots(edge);
        for (std::size_t index = 0; index < count; ++index)
            edges(pending[index], edge);
    }

    // Reclaim only after the final mark has validated every reachable edge
    // and no source construction checkpoints or borrowed pointers are live.
    template<class Discard>
    void reclaimUnreachable(const Discard& discard) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Discard, const void*, std::size_t>);
        std::size_t survivors = 0;
        for (const auto* address : allocationOrder_) {
            const auto found = objects_.find(address);
            auto& allocation = found->second;
            if (allocation.marked) {
                allocation.index = survivors;
                allocationOrder_[survivors++] = address;
            } else {
                discard(address, allocation.words);
                usedWords_ -= allocation.words;
                objects_.erase(found);
            }
        }
        allocationOrder_.resize(survivors);
    }

    void restore(std::size_t checkpoint) noexcept
    {
        restore(checkpoint, [](const void*, std::size_t) noexcept {});
    }

    template<class Discard>
    void restore(std::size_t checkpoint, const Discard& discard) noexcept
    {
        while (allocationOrder_.size() > checkpoint) {
            const auto allocation = objects_.find(allocationOrder_.back());
            discard(allocation->first, allocation->second.words);
            activePins_ -= allocation->second.pins;
            usedWords_ -= allocation->second.words;
            objects_.erase(allocation);
            allocationOrder_.pop_back();
        }
    }

    void* allocate(std::uint64_t words, std::uint64_t tag) noexcept
    {
        if (nextIdentity_ == std::numeric_limits<std::uint64_t>::max() ||
            words > maximumReceiverWords || objects_.size() >= model::ObjectDomain::maximumCells ||
            words + receiverHeaderWords > maximumWords - usedWords_)
            return nullptr;

        const auto size = words + receiverHeaderWords;
        try {
            auto storage = std::make_unique<std::uint64_t[]>(size);
            storage[receiverSizeWord] = words;
            storage[receiverTagWord] = tag;
            auto* receiver = storage.get();
            const auto index = allocationOrder_.size();
            allocationOrder_.push_back(receiver);
            try {
                objects_.emplace(receiver, Allocation{std::move(storage), index, size, nextIdentity_++});
            } catch (...) {
                allocationOrder_.pop_back();
                throw;
            }
            usedWords_ += size;
            return receiver;
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }

private:
    struct Allocation {
        std::unique_ptr<std::uint64_t[]> storage;
        std::size_t index;
        std::size_t words;
        std::uint64_t identity;
        std::size_t pins = 0;
        bool marked = false;
    };

    std::unordered_map<const void*, Allocation> objects_;
    std::vector<const void*> allocationOrder_;
    std::size_t usedWords_ = 0;
    std::uint64_t nextIdentity_ = 1;
    std::size_t activePins_ = 0;
};

}
