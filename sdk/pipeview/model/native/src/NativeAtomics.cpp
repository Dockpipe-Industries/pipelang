// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeAtomics.hpp"

#include <pipelang/model/atomic.hpp>
#include <pipelang/model/object.hpp>

#include <bit>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace pipelang::native_app_v19 {

class NativeAtomicCell {
public:
    using Value = std::variant<model::Atomic<std::int64_t>, model::Atomic<bool>, model::Atomic<double>,
                               model::Atomic<float>>;

    template<class T>
    NativeAtomicCell(model::Atomic<T> cell) : value(std::move(cell)) {}

    Value value;
};

namespace {

using model::Atomic;
using model::EvaluationBudget;
using model::ExecutionFailure;
using model::ObjectDomain;

template<class Value>
Value decodeAtomicWord(std::uint64_t word)
{
    if constexpr (std::is_same_v<Value, bool>)
        return word != 0;
    else if constexpr (std::is_same_v<Value, float>)
        return std::bit_cast<float>(static_cast<std::uint32_t>(word));
    else
        return std::bit_cast<Value>(word);
}

template<class Value>
std::uint64_t encodeAtomicWord(Value value)
{
    if constexpr (std::is_same_v<Value, bool>)
        return value ? 1 : 0;
    else if constexpr (std::is_same_v<Value, float>)
        return std::bit_cast<std::uint32_t>(value);
    else
        return std::bit_cast<std::uint64_t>(value);
}

template<class Value>
std::uint64_t callAtomic(const Atomic<Value>& cell, AtomicOperation operation, std::uint64_t argument,
                         std::uint64_t comparand, EvaluationBudget& budget)
{
    switch (operation) {
    case AtomicOperation::Load:
        return encodeAtomicWord(cell.load());
    case AtomicOperation::Store:
        cell.store(decodeAtomicWord<Value>(argument));
        return 0;
    case AtomicOperation::Exchange:
        return encodeAtomicWord(cell.exchange(decodeAtomicWord<Value>(argument)));
    case AtomicOperation::CompareExchange:
        return encodeAtomicWord(cell.compareExchange(decodeAtomicWord<Value>(argument),
                                                     decodeAtomicWord<Value>(comparand)));
    case AtomicOperation::Add:
        if constexpr (std::is_same_v<Value, std::int64_t>)
            return encodeAtomicWord(cell.add(decodeAtomicWord<Value>(argument), &budget));
        break;
    }
    throw ExecutionFailure("invalid native atomic operation");
}

NativeAtomicCell createAtomicCell(AtomicValueKind kind, std::uint64_t value)
{
    switch (kind) {
    case AtomicValueKind::Integer:
        return Atomic<std::int64_t>::Create(decodeAtomicWord<std::int64_t>(value));
    case AtomicValueKind::Boolean:
        return Atomic<bool>::Create(decodeAtomicWord<bool>(value));
    case AtomicValueKind::Float:
        return Atomic<double>::Create(decodeAtomicWord<double>(value));
    case AtomicValueKind::Float32:
        return Atomic<float>::Create(decodeAtomicWord<float>(value));
    }
    throw ExecutionFailure("invalid native atomic value type");
}

} // namespace

std::uint64_t callAtomicCell(const NativeAtomicCell& cell, AtomicValueKind kind, AtomicOperation operation,
                                   std::uint64_t argument, std::uint64_t comparand, EvaluationBudget& budget)
{
    if (cell.value.index() != static_cast<std::size_t>(kind))
        throw ExecutionFailure("native atomic handle has the wrong value type");
    return std::visit([&](const auto& value) {
        return callAtomic(value, operation, argument, comparand, budget);
    }, cell.value);
}

struct LocalAtomics::Entry {
    std::optional<NativeAtomicCell> cell;
};

LocalAtomics::LocalAtomics() = default;
LocalAtomics::~LocalAtomics() = default;

std::uint64_t LocalAtomics::initialize(std::size_t site, AtomicValueKind kind, std::uint64_t value,
                                       model::EvaluationBudget& budget)
{
    if (site >= maximumFrameWords)
        throw ExecutionFailure("native local atomic storage limit exceeded");
    if (site >= cells_.size())
        cells_.resize(site + 1);
    auto& cell = cells_[site].cell;
    if (cell)
        callAtomicCell(*cell, kind, AtomicOperation::Store, value, 0, budget);
    else
        cell = createAtomicCell(kind, value);
    return handleTag | site;
}

const NativeAtomicCell& LocalAtomics::at(std::uint64_t handle) const
{
    const auto site = handle & ~handleTag;
    if (site >= cells_.size() || !cells_[site].cell)
        throw ExecutionFailure("native atomic does not belong to the current frame");
    return *cells_[site].cell;
}

InvocationAtomics::InvocationAtomics() = default;
InvocationAtomics::~InvocationAtomics() = default;

std::uint64_t InvocationAtomics::borrowCell(const InvocationAtomics& parent, std::uint64_t handle)
{
    return borrowCell(parent.at(handle));
}

std::uint64_t InvocationAtomics::borrowCell(const NativeAtomicCell& cell)
{
    return cells_.retain(cell, "native invocation atomic storage limit exceeded") + 1;
}

std::uint64_t InvocationAtomics::borrowCell(const HostedAtomic& cell)
{
    return std::visit([&](const auto& value) {
        value.requireCell();
        return borrowCell(NativeAtomicCell(value));
    }, cell);
}

std::size_t InvocationAtomics::checkpoint() const noexcept
{
    return cells_.checkpoint();
}

void InvocationAtomics::restore(std::size_t checkpoint) noexcept
{
    cells_.restore(checkpoint);
}

void InvocationAtomics::retire(std::uint64_t handle) noexcept
{
    cells_.retire(handle - 1);
}

std::uint64_t InvocationAtomics::create(AtomicValueKind kind, std::uint64_t value)
{
    return cells_.retain(createAtomicCell(kind, value), "native invocation atomic storage limit exceeded") + 1;
}

const NativeAtomicCell& InvocationAtomics::at(std::uint64_t handle) const
{
    const auto* cell = cells_.find(handle - 1);
    if (!cell)
        throw ExecutionFailure("native atomic does not belong to the current invocation");
    return *cell;
}

}
