// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "result.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pipelang::model {
inline constexpr std::size_t maximumListElements = 4096;
inline constexpr std::size_t maximumListBytes = 1024 * 1024;

// Return cap + 1 as a stable saturation sentinel. Never add before checking.
inline std::size_t boundedOwnedSum(std::size_t left, std::size_t right,
                                   std::size_t cap) noexcept {
    if (left > cap || right > cap || right > cap - left)
        return cap + 1;
    return left + right;
}

template<class T> class List;
// These declarations must precede every wrapper's dependent recursive calls in
// result.hpp too. Definitions there must recurse using unqualified calls so ADL
// finds generated record owners. A declaration here alone cannot fix lookup in
// an earlier wrapper definition.
template<class T> std::size_t ownedBytes(const T& value);
inline std::size_t ownedBytes(const std::string& value);
inline std::size_t ownedBytes(const Error& value);
template<class T> std::size_t ownedBytes(const Optional<T>& value);
template<class T, class E> std::size_t ownedBytes(const Result<T, E>& value);
template<class T> std::size_t ownedBytes(const List<T>& value);
template<class T> std::size_t ownedElements(const T& value);
template<class T> std::size_t ownedElements(const Optional<T>& value);
template<class T, class E> std::size_t ownedElements(const Result<T, E>& value);
template<class T> std::size_t ownedElements(const List<T>& value);
template<class T> void validate(const List<T>& value);

template<class T>
std::size_t ownedBytes(const T&) { return sizeof(T); }
inline std::size_t ownedBytes(const std::string& value) {
    return boundedOwnedSum(sizeof(std::string), value.size(), maximumListBytes);
}
inline std::size_t ownedBytes(const Error& value) {
    return boundedOwnedSum(sizeof(value), ownedBytes(value.message), maximumListBytes);
}
template<class T>
std::size_t ownedElements(const T&) { return 0; }

// Footprint is a deterministic conservative logical ownership cost: the list
// object plus each element's recursively owned cost. Capacity/allocator overhead
// is deliberately excluded; it is not a process RSS guarantee. Generated record
// overloads count sizeof(record) plus recursively owned field costs. Wrapper
// overloads count their object size plus the active payload/error costs.
template<class T>
std::size_t ownedBytes(const Optional<T>& value) {
    return value.has_value() ? boundedOwnedSum(sizeof(value), ownedBytes(value.value()), maximumListBytes) : sizeof(value);
}
template<class T, class E>
std::size_t ownedBytes(const Result<T, E>& value) {
    auto bytes = boundedOwnedSum(sizeof(value), ownedBytes(value.error()), maximumListBytes);
    if constexpr (!std::is_void_v<T>) {
        if (value.is_success())
            bytes = boundedOwnedSum(bytes, ownedBytes(value.value()), maximumListBytes);
    }
    return bytes;
}
template<class T>
std::size_t ownedElements(const Optional<T>& value) {
    return value.has_value() ? ownedElements(value.value()) : 0;
}
template<class T, class E>
std::size_t ownedElements(const Result<T, E>& value) {
    auto elements = ownedElements(value.error());
    if constexpr (!std::is_void_v<T>) {
        if (value.is_success())
            elements = boundedOwnedSum(elements, ownedElements(value.value()), maximumListElements);
    }
    return elements;
}

inline std::size_t addOwnedBytes(std::size_t left, std::size_t right) {
    return boundedOwnedSum(left, right, maximumListBytes);
}

class EvaluationBudget {
public:
    class CleanupRegion {
    public:
        explicit CleanupRegion(EvaluationBudget& budget) : budget_(budget) { ++budget_.cleanupDepth_; }
        CleanupRegion(const CleanupRegion&) = delete;
        CleanupRegion& operator=(const CleanupRegion&) = delete;
        ~CleanupRegion() { --budget_.cleanupDepth_; }
    private:
        EvaluationBudget& budget_;
    };
    void step() {
        auto& remaining = cleanupDepth_ == 0 ? remaining_ : cleanupRemaining_;
        if (remaining == 0)
            throw EvaluationLimitFailure(cleanupDepth_ == 0
                ? "source evaluation budget exceeded" : "cleanup evaluation budget exceeded");
        --remaining;
    }
    CleanupRegion cleanup() { return CleanupRegion(*this); }
    void reset() {
        remaining_ = maximumSteps;
        cleanupRemaining_ = maximumSteps;
    }
    static constexpr std::size_t maximumSteps = 65536;
private:
    std::size_t remaining_ = maximumSteps;
    std::size_t cleanupRemaining_ = maximumSteps;
    unsigned cleanupDepth_ = 0;
};

template<class T>
class List {
public:
    static List Empty() { return {}; }
    void add(T value) {
        using pipelang::model::validate;
        validate(value);
        using pipelang::model::ownedBytes;
        using pipelang::model::ownedElements;
        const auto elementCost = boundedOwnedSum(1, ownedElements(value), maximumListElements);
        const auto nextElements = boundedOwnedSum(elements_, elementCost, maximumListElements);
        const auto nextBytes = boundedOwnedSum(bytes_, ownedBytes(value), maximumListBytes);
        if (nextElements > maximumListElements)
            throw ExecutionFailure("list aggregate element limit exceeded");
        if (nextBytes > maximumListBytes)
            throw ExecutionFailure("list owned byte limit exceeded");
        // vector append has the strong exception guarantee for compiler-admitted
        // copyable owned values. Cost counters change only after append succeeds.
        values_.push_back(std::move(value));
        elements_ = nextElements;
        bytes_ = nextBytes;
    }
    std::int64_t count() const { return static_cast<std::int64_t>(values_.size()); }
    T at(std::int64_t index) const {
        if (index < 0 || static_cast<std::uint64_t>(index) >= values_.size())
            throw ExecutionFailure("list index outside admitted range");
        return values_[static_cast<std::size_t>(index)];
    }
    const std::vector<T>& values() const { return values_; }
    std::size_t ownedByteCost() const { return bytes_; }
    std::size_t ownedElementCost() const { return elements_; }
    bool operator==(const List& other) const { return values_ == other.values_; }
private:
    std::vector<T> values_;
    std::size_t bytes_ = sizeof(List);
    std::size_t elements_ = 0;
};
template<class T>
std::size_t ownedBytes(const List<T>& value) { return value.ownedByteCost(); }
template<class T>
std::size_t ownedElements(const List<T>& value) { return value.ownedElementCost(); }
template<class T>
void validate(const List<T>& value) {
    // Recurse even though normal insertion already validates: host validation
    // and generated record validation have the same owning boundary.
    using pipelang::model::validate;
    for (const auto& item : value.values())
        validate(item);
    if (value.ownedByteCost() > maximumListBytes ||
        value.ownedElementCost() > maximumListElements)
        throw ExecutionFailure("list ownership budget exceeded");
}
}
