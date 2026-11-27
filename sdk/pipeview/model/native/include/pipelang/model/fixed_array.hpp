// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace pipelang::model {

// A fixed owned value, independent of the dynamic Array implementation. Source
// checks both its element ownership and its complete expanded inline footprint.
template<class T, std::size_t N>
class FixedArray {
public:
    static_assert(N <= maximumListElements, "fixed array extent exceeds the collection element limit");

    constexpr FixedArray() : values_(defaultValues(std::make_index_sequence<N>{})) {}

    static FixedArray Create(T initial) {
        using pipelang::model::ownedBytes;
        using pipelang::model::ownedElements;
        using pipelang::model::validate;
        validate(initial);
        std::size_t bytes = sizeof(FixedArray);
        std::size_t elements = 0;
        const auto elementBytes = ownedBytes(initial);
        const auto elementCount = boundedOwnedSum(1, ownedElements(initial), maximumListElements);
        for (std::size_t index = 0; index < N; ++index) {
            bytes = boundedOwnedSum(bytes, elementBytes, maximumListBytes);
            elements = boundedOwnedSum(elements, elementCount, maximumListElements);
        }
        requireOwnership(bytes, elements);
        return filledUnchecked(initial);
    }

    // Only generated native code with a closed-value proof uses this factory.
    // Construct each element directly: no element default runs before its fill.
    static constexpr FixedArray filledUnchecked(const T& initial)
        noexcept(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T>) {
        return filledUnchecked(initial, std::make_index_sequence<N>{});
    }

    constexpr std::int64_t count() const noexcept {
        return static_cast<std::int64_t>(N);
    }

    constexpr T at(std::int64_t index) const {
        return values_[checkedIndex(index)];
    }

    void set(std::int64_t index, T value) {
        const auto position = checkedIndex(index);
        using pipelang::model::ownedBytes;
        using pipelang::model::ownedElements;
        using pipelang::model::validate;
        validate(value);
        std::size_t bytes = sizeof(FixedArray);
        std::size_t elements = 0;
        for (std::size_t item = 0; item < N; ++item) {
            const auto& prospective = item == position ? value : values_[item];
            bytes = boundedOwnedSum(bytes, ownedBytes(prospective), maximumListBytes);
            const auto count = boundedOwnedSum(1, ownedElements(prospective), maximumListElements);
            elements = boundedOwnedSum(elements, count, maximumListElements);
        }
        // Expected ownership failures leave the previous value intact.
        requireOwnership(bytes, elements);
        values_[position] = std::move(value);
    }

    constexpr std::array<T, N> values() const
        noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return values_;
    }

    // Source has no unchecked access syntax. The numeric compiler selects these
    // references only after proving bounds and preserving the owning snapshot.
    constexpr const T& uncheckedAt(std::size_t index) const noexcept {
        return values_[index];
    }

    constexpr T& uncheckedAt(std::size_t index) noexcept {
        return values_[index];
    }

    constexpr auto begin() noexcept {
        return values_.begin();
    }

    constexpr auto end() noexcept {
        return values_.end();
    }

    constexpr auto begin() const noexcept {
        return values_.begin();
    }

    constexpr auto end() const noexcept {
        return values_.end();
    }

    bool operator==(const FixedArray&) const = default;

private:
    static constexpr T defaultElement(std::size_t& bytes, std::size_t& elements) {
        T value{};
        using pipelang::model::ownedBytes;
        using pipelang::model::ownedElements;
        using pipelang::model::validate;
        validate(value);
        const auto nextBytes = boundedOwnedSum(bytes, ownedBytes(value), maximumListBytes);
        const auto addedElements = boundedOwnedSum(1, ownedElements(value), maximumListElements);
        const auto nextElements = boundedOwnedSum(elements, addedElements, maximumListElements);
        requireOwnership(nextBytes, nextElements);
        bytes = nextBytes;
        elements = nextElements;
        return value;
    }

    template<std::size_t... Indices>
    static constexpr std::array<T, N> defaultValues(std::index_sequence<Indices...>) {
        std::size_t bytes = sizeof(FixedArray);
        std::size_t elements = 0;
        requireOwnership(bytes, elements);
        // Braced initialization preserves element order and stops immediately
        // on a default/validation/ownership failure. An empty pack creates no T.
        // Keep a named owner for partial initialization so an exception in a
        // later default reliably destroys the elements already constructed.
        std::array<T, N> values{(static_cast<void>(Indices), defaultElement(bytes, elements))...};
        return values;
    }

    constexpr explicit FixedArray(std::array<T, N> values)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        : values_(std::move(values)) {
    }

    template<std::size_t... Indices>
    static constexpr FixedArray filledUnchecked(const T& initial, std::index_sequence<Indices...>)
        noexcept(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T>) {
        return FixedArray(std::array<T, N>{(static_cast<void>(Indices), initial)...});
    }

    static constexpr std::size_t checkedIndex(std::int64_t index) {
        if (index < 0 || static_cast<std::uint64_t>(index) >= N)
            throw ExecutionFailure("collection index outside admitted range");
        return static_cast<std::size_t>(index);
    }

    static constexpr void requireOwnership(std::size_t bytes, std::size_t elements) {
        if (elements > maximumListElements)
            throw ExecutionFailure("collection aggregate element limit exceeded");
        if (bytes > maximumListBytes)
            throw ExecutionFailure("collection owned byte limit exceeded");
    }

    std::array<T, N> values_{};
};

template<class T, std::size_t N>
constexpr std::size_t ownedBytes(const FixedArray<T, N>& value) {
    using pipelang::model::ownedBytes;
    std::size_t bytes = sizeof(value);
    for (const auto& item : value)
        bytes = boundedOwnedSum(bytes, ownedBytes(item), maximumListBytes);
    return bytes;
}

template<class T, std::size_t N>
constexpr std::size_t ownedElements(const FixedArray<T, N>& value) {
    using pipelang::model::ownedElements;
    std::size_t elements = N;
    for (const auto& item : value)
        elements = boundedOwnedSum(elements, ownedElements(item), maximumListElements);
    return elements;
}

template<class T, std::size_t N>
constexpr void validate(const FixedArray<T, N>& value) {
    using pipelang::model::validate;
    for (const auto& item : value)
        validate(item);
    if (ownedElements(value) > maximumListElements)
        throw ExecutionFailure("collection aggregate element limit exceeded");
    if (ownedBytes(value) > maximumListBytes)
        throw ExecutionFailure("collection owned byte limit exceeded");
}

}
