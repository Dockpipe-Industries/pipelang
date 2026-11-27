// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeConcurrentSequences.hpp"

#include "NativeConcurrentValue.hpp"
#include "NativeLists.hpp"
#include "native_host_projection.hpp"

#include <pipelang/model/concurrent_collections.hpp>
#include <pipelang/model/object.hpp>

#include <bit>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace pipelang::native_app_v19 {
namespace {

using Queue = model::ConcurrentQueue<NativeConcurrentValue>;
using Stack = model::ConcurrentStack<NativeConcurrentValue>;
using IntegerQueue = model::ConcurrentQueue<std::int64_t>;
using HostedSequence = std::shared_ptr<const detail::HostedSequenceCell>;

HostedValue projectElement(const NativeConcurrentValue& value)
{
    return value.value;
}

HostedValue projectElement(std::int64_t value)
{
    return HostedValue::fromInteger(value);
}

}

struct NativeConcurrentSequences::Entry {
    std::variant<Queue, Stack, IntegerQueue, HostedSequence> cell;
    const OwnedShape* element;
};

NativeConcurrentSequences::NativeConcurrentSequences() = default;
NativeConcurrentSequences::~NativeConcurrentSequences() = default;

std::size_t NativeConcurrentSequences::checkpoint() const noexcept
{
    return cells_.checkpoint();
}

void NativeConcurrentSequences::restore(std::size_t checkpoint) noexcept
{
    cells_.restore(checkpoint);
}

void NativeConcurrentSequences::retire(std::uint64_t handle) noexcept
{
    cells_.retire(handle - 1);
}

std::uint64_t NativeConcurrentSequences::retain(Entry entry)
{
    return cells_.retain(std::move(entry), "native concurrent sequence storage limit exceeded") + 1;
}

std::uint64_t NativeConcurrentSequences::create(ConcurrentSequenceKind kind, const OwnedShape& element)
{
    switch (kind) {
    case ConcurrentSequenceKind::Queue: return retain({Queue::Empty(), &element});
    case ConcurrentSequenceKind::Stack: return retain({Stack::Empty(), &element});
    }
    throw model::ExecutionFailure("invalid native concurrent sequence type");
}

std::uint64_t NativeConcurrentSequences::borrow(const NativeConcurrentSequences& parent, std::uint64_t handle)
{
    return retain(parent.at(handle));
}

std::uint64_t NativeConcurrentSequences::borrow(IntegerQueue queue)
{
    queue.requireCell();
    return retain({std::move(queue), nullptr});
}

std::uint64_t NativeConcurrentSequences::borrow(const HostedValue& sequence)
{
    return retain({sequence.hostedSequence(), nullptr});
}

const NativeConcurrentSequences::Entry& NativeConcurrentSequences::at(std::uint64_t handle) const
{
    const auto* cell = cells_.find(handle - 1);
    if (!cell)
        throw model::ExecutionFailure("native concurrent sequence does not belong to the invocation");
    return *cell;
}

void NativeConcurrentSequences::call(std::uint64_t handle, ConcurrentSequenceOperation operation,
                                     const OwnedShape& element, const std::uint64_t* input,
                                     const OwnedShape* result, std::uint64_t* output, NativeLists& lists,
                                     HostedInput& packing) const
{
    const auto& entry = at(handle);
    const auto* hosted = std::get_if<HostedSequence>(&entry.cell);
    bool matchesElement = entry.element == &element;
    if (hosted)
        matchesElement = element.name && (*hosted)->elementType() == element.name;
    else if (!entry.element)
        matchesElement = element.kind == OwnedKind::Integer && element.words == 1;
    if (!matchesElement)
        throw model::ExecutionFailure("native concurrent sequence element type does not match its cell");

    const auto project = [&](const auto& value) {
        if (!result || !output)
            throw model::ExecutionFailure("native concurrent sequence result storage is missing");
        const auto projected = HostedValue::fromOptional(std::string(result->name),
            value.has_value() ? projectElement(value.value()) : HostedValue{});
        packing.pack(*result, projected, output, &lists);
    };

    if (hosted) {
        const auto& cell = **hosted;
        const auto projectOptional = [&](const std::optional<HostedValue>& value) {
            if (!result || !output)
                throw model::ExecutionFailure("native concurrent sequence result storage is missing");
            packing.pack(*result, HostedValue::fromOptional(std::string(result->name), value.value_or(HostedValue{})),
                         output, &lists);
        };
        switch (operation) {
        case ConcurrentSequenceOperation::Append:
            if (!input)
                throw model::ExecutionFailure("native concurrent sequence input is missing");
            return cell.append(HostedProjection(lists).copy(element, input));
        case ConcurrentSequenceOperation::TryTake: return cell.take(projectOptional);
        case ConcurrentSequenceOperation::TryPeek: return projectOptional(cell.peek());
        case ConcurrentSequenceOperation::Clear: return cell.clear();
        case ConcurrentSequenceOperation::Count:
            if (!output)
                throw model::ExecutionFailure("native concurrent sequence count storage is missing");
            *output = static_cast<std::uint64_t>(cell.count());
            return;
        case ConcurrentSequenceOperation::Items:
            if (!result || !output)
                throw model::ExecutionFailure("native concurrent sequence snapshot storage is missing");
            return packing.pack(*result, HostedValue::fromList(std::string(result->name), cell.items()), output, &lists);
        }
        throw model::ExecutionFailure("invalid native concurrent sequence operation");
    }

    std::visit([&](const auto& sequence) {
        using Sequence = std::decay_t<decltype(sequence)>;
        if constexpr (!std::is_same_v<Sequence, HostedSequence>) {
            switch (operation) {
            case ConcurrentSequenceOperation::Count:
                if (!output)
                    throw model::ExecutionFailure("native concurrent sequence count storage is missing");
                *output = static_cast<std::uint64_t>(sequence.count());
                break;
            case ConcurrentSequenceOperation::Append: {
                if (!input)
                    throw model::ExecutionFailure("native concurrent sequence input is missing");
                if constexpr (std::is_same_v<Sequence, IntegerQueue>) {
                    sequence.enqueue(std::bit_cast<std::int64_t>(*input));
                } else {
                    HostedProjection projection(lists);
                    NativeConcurrentValue retained{projection.copy(element, input), lists.ownership(element, input)};
                    if constexpr (std::is_same_v<Sequence, Stack>)
                        sequence.push(std::move(retained));
                    else
                        sequence.enqueue(std::move(retained));
                }
                break;
            }
            case ConcurrentSequenceOperation::TryTake:
                if constexpr (std::is_same_v<Sequence, Stack>)
                    sequence.tryPopProjected(project);
                else
                    sequence.tryDequeueProjected(project);
                break;
            case ConcurrentSequenceOperation::TryPeek:
                project(sequence.tryPeek());
                break;
            case ConcurrentSequenceOperation::Clear:
                sequence.clear();
                break;
            case ConcurrentSequenceOperation::Items: {
                if (!result || !output)
                    throw model::ExecutionFailure("native concurrent sequence snapshot storage is missing");
                const auto snapshot = sequence.items();
                std::vector<HostedValue> values;
                values.reserve(static_cast<std::size_t>(snapshot.count()));
                for (const auto& item : snapshot.values())
                    values.push_back(projectElement(item));
                packing.pack(*result, HostedValue::fromList(std::string(result->name), std::move(values)), output, &lists);
                break;
            }
            default:
                throw model::ExecutionFailure("invalid native concurrent sequence operation");
            }
        }
    }, entry.cell);
}

}
