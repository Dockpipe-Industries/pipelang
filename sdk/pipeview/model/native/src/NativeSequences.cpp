// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeLists.hpp"

namespace pipelang::native_app_v19 {

ListView NativeLists::sequencePrefix(SequenceView view, const OwnedShape& element) const
{
    if (view.first > 2 * model::maximumListElements || view.count > model::maximumListElements ||
        (view.count == 0 && (view.first != 0 || view.block != 0)))
        throw model::ExecutionFailure("invalid native sequence range");
    const ListView prefix{view.block, view.first + view.count};
    (void)block(prefix, element);
    return prefix;
}

model::ListOwnership NativeLists::sequenceCost(SequenceView view, const OwnedShape& shape) const
{
    const auto& element = *shape.fields[0].shape;
    const auto prefix = sequencePrefix(view, element);
    model::ListOwnership ownership{referenceLayout(shape).size, 0};
    if (const auto* previous = block(prefix, element)) {
        const auto before = previous->costAt(view.first);
        const auto after = previous->costAt(prefix.count);
        ownership.bytes += after.bytes - before.bytes;
        ownership.elements = after.elements - before.elements;
    }
    return ownership;
}

std::span<const std::byte> NativeLists::sequenceElement(SequenceView view, const OwnedShape& shape,
                                                       std::int64_t index) const
{
    const auto& element = *shape.fields[0].shape;
    const auto bytes = borrowPrefix(sequencePrefix(view, element), element);
    auto position = model::collections_detail::indexOf(index, view.count);
    if (shape.kind == OwnedKind::Stack)
        position = view.count - position - 1;
    const auto stride = element.words * sizeof(std::uint64_t);
    return bytes.subspan((view.first + position) * stride, stride);
}

SequenceView NativeLists::appendSequence(SequenceView view, const OwnedShape& shape, const void* value,
                                         std::size_t protectedBlocks)
{
    const auto& element = *shape.fields[0].shape;
    const auto prefix = sequencePrefix(view, element);
    const auto* previous = block(prefix, element);
    const auto current = sequenceCost(view, shape);
    const auto* data = static_cast<const std::byte*>(value);
    const auto item = cost(element, data);
    const auto elements = model::boundedOwnedSum(1, item.elements, model::maximumListElements);
    (void)model::collections_detail::appendOwnership({current.bytes, current.elements}, {item.bytes, elements});
    const auto stride = element.words * sizeof(std::uint64_t);
    const auto growth = stride + sizeof(model::ListOwnership);

    const auto base = empty(element);
    const auto before = previous ? previous->costAt(view.first) : base;
    const auto after = previous ? previous->costAt(prefix.count) : base;
    // Leave old snapshots on the original block. Copy only the live range
    // when appending to a branch or when the dead prefix outweighs the range.
    if (previous && view.block > protectedBlocks && prefix.count == previous->prefixes.size() &&
        view.first < view.count &&
        before.bytes - base.bytes <= current.bytes) {
        admitArena(growth);
        blocks_[view.block - 1].append(data, stride, {after.bytes + item.bytes, after.elements + elements});
        bytes_ += growth;
        return {view.block, view.count + 1, view.first};
    }

    const auto allocation = (view.count + 1) * growth + sizeof(Block);
    admitArena(allocation);
    Block fork{&element, {}, {}};
    if (previous) {
        fork.bytes.assign(previous->bytes.begin() + view.first * stride,
                          previous->bytes.begin() + prefix.count * stride);
        fork.prefixes.reserve(view.count + 1);
        for (auto index = view.first; index < prefix.count; ++index) {
            const auto owned = previous->prefixes[index];
            fork.prefixes.push_back({base.bytes + (owned.bytes - before.bytes), owned.elements - before.elements});
        }
    }
    fork.append(data, stride, {base.bytes + (after.bytes - before.bytes) + item.bytes,
                               after.elements - before.elements + elements});
    blocks_.push_back(std::move(fork));
    bytes_ += allocation;
    return {blocks_.size(), view.count + 1, 0};
}

}
