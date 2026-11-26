// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/extended_collections.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeArrays {
public:
    using Ownership = model::collections_detail::Ownership;

    struct Checkpoint {
        std::size_t nodes;
        std::size_t values;
        std::size_t versions;
    };

    Checkpoint checkpoint() const noexcept
    {
        return {nodes_.size(), values_.size(), versions_.size()};
    }

    void restore(Checkpoint checkpoint) noexcept
    {
        versions_.resize(checkpoint.versions);
        nodes_.resize(checkpoint.nodes);
        values_.resize(checkpoint.values);
    }

    ArrayView create(const OwnedShape& element, std::size_t count, const void* value, Ownership item)
    {
        if (count == 0)
            return {};

        const auto total = model::collections_detail::appendOwnership(empty(), {
            repeat(item.bytes, count, model::maximumListBytes),
            repeat(item.elements, count, model::maximumListElements)
        });
        reserve(1, element.words);
        return publish(element, addUniform(appendValue(element, value)), count, total);
    }

    ArrayView import(const OwnedShape& element, std::span<const std::uint64_t> values, std::size_t count,
                     Ownership total)
    {
        if (count == 0)
            return {};
        reserve(2 * count - 1, values.size());
        const auto first = values_.size();
        values_.insert(values_.end(), values.begin(), values.end());
        return publish(element, build(element.words, first, count), count, total);
    }

    std::span<const std::byte> at(ArrayView view, const OwnedShape& element, std::int64_t index) const
    {
        const auto position = model::collections_detail::indexOf(index, view.count);
        const auto version = checked(view, element);
        const auto offset = locate(version.node, version.count, position);
        return std::as_bytes(std::span(values_).subspan(offset, element.words));
    }

    ArrayView replace(ArrayView view, const OwnedShape& element, std::int64_t index, const void* value,
                      Ownership previous, Ownership replacement)
    {
        const auto position = model::collections_detail::indexOf(index, view.count);
        const auto version = checked(view, element);
        const Ownership remainder{version.ownership.bytes - previous.bytes,
                                  version.ownership.elements - previous.elements};
        const auto total = model::collections_detail::appendOwnership(remainder, replacement);
        reserve(std::bit_width(version.count - 1) + 1, element.words);
        const auto fresh = addUniform(appendValue(element, value));
        const auto root = update(version.node, version.count, position, fresh);
        return publish(element, root, version.count, total);
    }

    Ownership ownership(ArrayView view, const OwnedShape& element) const
    {
        return checked(view, element).ownership;
    }

    static constexpr Ownership empty() { return {sizeof(model::Array<std::int64_t>), 0}; }

private:
    // A uniform node represents any extent of the same element. Updates copy
    // only the path to one position, sharing the untouched subtrees.
    struct Node {
        std::size_t left;
        std::size_t right;
        std::size_t offset;
    };

    struct Version {
        const OwnedShape* element;
        std::size_t node;
        std::size_t count;
        Ownership ownership;
    };

    static std::size_t repeat(std::size_t value, std::size_t count, std::size_t limit)
    {
        return value > limit / count ? limit + 1 : value * count;
    }

    template<class T>
    static void ensureCapacity(std::vector<T>& storage, std::size_t added)
    {
        const auto required = storage.size() + added;
        if (required > storage.capacity())
            storage.reserve(std::max(required, storage.capacity() + storage.capacity() / 2));
    }

    void reserve(std::size_t nodes, std::size_t words)
    {
        constexpr auto maximumBytes = 32 * model::maximumListBytes;
        const auto total = (nodes_.size() + nodes) * sizeof(Node) +
            (values_.size() + words) * sizeof(std::uint64_t) + (versions_.size() + 1) * sizeof(Version);
        if (total > maximumBytes)
            throw model::EvaluationLimitFailure("native invocation array storage limit exceeded");
        ensureCapacity(nodes_, nodes);
        ensureCapacity(values_, words);
        ensureCapacity(versions_, 1);
    }

    std::size_t appendValue(const OwnedShape& element, const void* value)
    {
        const auto offset = values_.size();
        values_.resize(offset + element.words);
        std::memcpy(values_.data() + offset, value, element.words * sizeof(std::uint64_t));
        return offset;
    }

    std::size_t addUniform(std::size_t offset)
    {
        nodes_.push_back({0, 0, offset});
        return nodes_.size();
    }

    std::size_t addBranch(std::size_t left, std::size_t right)
    {
        nodes_.push_back({left, right, 0});
        return nodes_.size();
    }

    std::size_t build(std::size_t stride, std::size_t offset, std::size_t count)
    {
        if (count == 1)
            return addUniform(offset);
        const auto leftCount = count / 2;
        const auto left = build(stride, offset, leftCount);
        const auto right = build(stride, offset + leftCount * stride, count - leftCount);
        return addBranch(left, right);
    }

    std::size_t locate(std::size_t node, std::size_t count, std::size_t position) const
    {
        while (nodes_[node - 1].left != 0) {
            const auto& branch = nodes_[node - 1];
            const auto leftCount = count / 2;
            if (position < leftCount) {
                node = branch.left;
                count = leftCount;
            } else {
                node = branch.right;
                position -= leftCount;
                count -= leftCount;
            }
        }
        return nodes_[node - 1].offset;
    }

    std::size_t update(std::size_t node, std::size_t count, std::size_t position, std::size_t replacement)
    {
        if (count == 1)
            return replacement;
        const auto previous = nodes_[node - 1];
        auto left = previous.left == 0 ? node : previous.left;
        auto right = previous.right == 0 ? node : previous.right;
        const auto leftCount = count / 2;
        if (position < leftCount)
            left = update(left, leftCount, position, replacement);
        else
            right = update(right, count - leftCount, position - leftCount, replacement);
        return addBranch(left, right);
    }

    Version checked(ArrayView view, const OwnedShape& element) const
    {
        if (view.version == 0 && view.count == 0)
            return {&element, 0, 0, empty()};
        if (view.version == 0 || view.version > versions_.size())
            throw model::ExecutionFailure("native array does not belong to the invocation");
        const auto& version = versions_[view.version - 1];
        if (version.element != &element || version.count != view.count)
            throw model::ExecutionFailure("native array element layout or extent is invalid");
        return version;
    }

    ArrayView publish(const OwnedShape& element, std::size_t node, std::size_t count, Ownership total)
    {
        versions_.push_back({&element, node, count, total});
        return {versions_.size(), count};
    }

    std::vector<Node> nodes_;
    std::vector<std::uint64_t> values_;
    std::vector<Version> versions_;
};
}
