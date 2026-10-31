// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"
#include "numeric.hpp"

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace pipelang::model {

// Copies explicitly share cell identity and lifetime. No reference to the cell
// or its ordinary mutable storage escapes this handle. Source admission prevents
// handles from entering owned snapshots, records, results or collections.
template<class T>
class Atomic {
    static_assert(std::is_same_v<T, std::int64_t> ||
                  std::is_same_v<T, bool> || std::is_same_v<T, double>,
                  "Atomic supports only int, bool and float64");
    using Storage = std::conditional_t<std::is_same_v<T, double>, std::uint64_t, T>;

public:
    Atomic() : Atomic(T{}) {
    }

    static Atomic Create(T value) {
        return Atomic(value);
    }

    T load() const {
        requireCell();
        return decode(cell_->load());
    }

    void store(T value) const {
        requireCell();
        cell_->store(encode(value));
    }

    T exchange(T value) const {
        requireCell();
        return decode(cell_->exchange(encode(value)));
    }

    T compareExchange(T value, T comparand) const {
        requireCell();
        auto observed = encode(comparand);
        cell_->compare_exchange_strong(observed, encode(value));
        return decode(observed);
    }

    T add(T delta, EvaluationBudget* budget = nullptr) const requires std::is_same_v<T, std::int64_t> {
        requireCell();
        auto observed = cell_->load();
        for (std::size_t attempt = 0; attempt < EvaluationBudget::maximumSteps; ++attempt) {
            if (budget)
                budget->step();
            // The checked calculation precedes CAS. Overflow leaves the shared
            // cell untouched; competing updates cause a fresh checked attempt.
            const auto next = pipelang::model::add(observed, delta);
            if (cell_->compare_exchange_strong(observed, next))
                return next;
        }
        throw EvaluationLimitFailure("atomic contention budget exceeded");
    }

    T increment(EvaluationBudget* budget = nullptr) const requires std::is_same_v<T, std::int64_t> {
        return add(1, budget);
    }

    T decrement(EvaluationBudget* budget = nullptr) const requires std::is_same_v<T, std::int64_t> {
        return add(-1, budget);
    }

    void requireCell() const {
        if (!cell_)
            throw ExecutionFailure("atomic handle has no cell");
    }

private:
    explicit Atomic(T value) : cell_(std::make_shared<std::atomic<Storage>>(encode(value))) {
    }

    static Storage encode(T value) {
        if constexpr (std::is_same_v<T, double>)
            return std::bit_cast<std::uint64_t>(value);
        else
            return value;
    }

    static T decode(Storage value) {
        if constexpr (std::is_same_v<T, double>)
            return std::bit_cast<double>(value);
        else
            return value;
    }

    std::shared_ptr<std::atomic<Storage>> cell_;
};

template<class T>
void validate(const Atomic<T>& value) {
    value.requireCell();
}

}
