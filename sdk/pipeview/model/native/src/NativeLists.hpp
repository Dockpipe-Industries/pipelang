// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"
#include "NativeArrays.hpp"
#include "NativeBytes.hpp"
#include "NativeDictionaries.hpp"
#include "NativeSets.hpp"

#include <pipelang/model/byte_buffer.hpp>
#include <pipelang/model/collection.hpp>
#include <pipelang/model/extended_collections.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeLists {
public:
    struct ConstructionCheckpoint {
        NativeArrays::Checkpoint arrays;
        NativeDictionaries::Checkpoint dictionaries;
        NativeSets::Checkpoint sets;
        std::size_t byteBlocks;
        std::size_t listBlocks;
        std::size_t listBytes;
    };

    ConstructionCheckpoint constructionCheckpoint() const noexcept
    {
        return {arrays_.checkpoint(), dictionaries_.checkpoint(), sets_.checkpoint(),
                byteBuffers_.checkpoint(), blocks_.size(), bytes_};
    }

    void restoreConstruction(ConstructionCheckpoint checkpoint) noexcept
    {
        dictionaries_.restore(checkpoint.dictionaries);
        sets_.restore(checkpoint.sets);
        blocks_.resize(checkpoint.listBlocks);
        bytes_ = checkpoint.listBytes;
        arrays_.restore(checkpoint.arrays);
        byteBuffers_.restore(checkpoint.byteBlocks);
    }

    model::ListOwnership ownership(const OwnedShape& shape, const void* value) const
    {
        return cost(shape, static_cast<const std::byte*>(value));
    }

    NativeSets& sets() { return sets_; }
    const NativeSets& sets() const { return sets_; }
    NativeBytes& bytes() { return byteBuffers_; }
    const NativeBytes& bytes() const { return byteBuffers_; }
    NativeDictionaries& dictionaries() { return dictionaries_; }
    const NativeDictionaries& dictionaries() const { return dictionaries_; }

    model::ListOwnership erasedResultOwnership(const OwnedShape& shape, const void* value) const
    {
        const auto payload = ownership(shape, value);
        ObjectLayout box{sizeof(void*), alignof(void*)};
        box.append(referenceLayout(shape));
        box.append({2 * sizeof(std::size_t), alignof(std::size_t)});
        const auto bytes = model::addOwnedBytes(2 * sizeof(void*), model::addOwnedBytes(box.finish().size, payload.bytes));
        const auto elements = model::boundedOwnedSum(1, payload.elements, model::maximumListElements);
        if (bytes > model::maximumListBytes || elements > model::maximumListElements)
            throw model::ExecutionFailure("result interface exceeds owned value limits");
        return {bytes, elements};
    }

    ArrayView createArray(const OwnedShape& element, std::int64_t count, const void* value)
    {
        if (count < 0 || count > static_cast<std::int64_t>(model::maximumListElements))
            throw model::ExecutionFailure("array size outside admitted range");
        return arrays_.create(element, static_cast<std::size_t>(count), value, arrayItemCost(element, value));
    }

    std::span<const std::byte> arrayElement(ArrayView view, const OwnedShape& element, std::int64_t index) const
    {
        return arrays_.at(view, element, index);
    }

    ArrayView replaceArray(ArrayView view, const OwnedShape& element, std::int64_t index, const void* value)
    {
        const auto previous = arrayItemCost(element, arrays_.at(view, element, index).data());
        const auto replacement = arrayItemCost(element, value);
        return arrays_.replace(view, element, index, value, previous, replacement);
    }

    template<class PackElement>
    ArrayView importArray(const OwnedShape& element, std::size_t count, PackElement pack)
    {
        if (count > model::maximumListElements)
            throw model::ExecutionFailure("array size outside admitted range");
        std::vector<std::uint64_t> staged;
        std::vector<std::uint64_t> item(element.words);
        auto total = NativeArrays::empty();
        for (std::size_t index = 0; index < count; ++index) {
            std::fill(item.begin(), item.end(), 0);
            pack(index, item.data());
            total = model::collections_detail::appendOwnership(total, arrayItemCost(element, item.data()));
            staged.insert(staged.end(), item.begin(), item.end());
        }
        return arrays_.import(element, staged, count, total);
    }

    void validateFixedArray(const OwnedShape& shape, const void* value) const
    {
        if (shape.kind != OwnedKind::FixedArray)
            throw model::ExecutionFailure("invalid native fixed array layout");
        const auto ownership = cost(shape, static_cast<const std::byte*>(value));
        if (ownership.elements > model::maximumListElements)
            throw model::ExecutionFailure("collection aggregate element limit exceeded");
        if (ownership.bytes > model::maximumListBytes)
            throw model::ExecutionFailure("collection owned byte limit exceeded");
    }

    ListView append(ListView list, const OwnedShape& shape, const void* value, std::size_t protectedBlocks = 0)
    {
        const auto* previous = block(list, shape);
        const auto prefix = previous ? previous->costAt(list.count) : empty(shape);
        const auto item = cost(shape, static_cast<const std::byte*>(value));
        const auto next = prefix.appended(item.bytes, item.elements);
        const auto* bytes = static_cast<const std::byte*>(value);
        const auto itemBytes = shape.words * sizeof(std::uint64_t);

        if (previous && list.block > protectedBlocks && list.count == previous->prefixes.size()) {
            admitArena(itemBytes + sizeof(model::ListOwnership));
            blocks_[list.block - 1].append(bytes, itemBytes, next);
            bytes_ += itemBytes + sizeof(model::ListOwnership);
            return {list.block, list.count + 1};
        }

        // A list value is an immutable prefix. Only appending to an older
        // branch or a block protected by a savepoint needs a copy.
        const auto growth = (list.count + 1) * (itemBytes + sizeof(model::ListOwnership)) + sizeof(Block);
        admitArena(growth);
        Block fork{&shape, {}, {}};
        if (previous) {
            fork.bytes.assign(previous->bytes.begin(), previous->bytes.begin() + list.count * itemBytes);
            fork.prefixes.assign(previous->prefixes.begin(), previous->prefixes.begin() + list.count);
        }
        fork.append(bytes, itemBytes, next);
        blocks_.push_back(std::move(fork));
        bytes_ += growth;
        return {blocks_.size(), list.count + 1};
    }

    void read(ListView list, const OwnedShape& shape, std::int64_t index, void* output) const
    {
        const auto content = borrowPrefix(list, shape);
        if (index < 0 || static_cast<std::uint64_t>(index) >= list.count)
            throw model::ExecutionFailure("list index outside admitted range");
        const auto itemBytes = shape.words * sizeof(std::uint64_t);
        std::memcpy(output, content.data() + static_cast<std::size_t>(index) * itemBytes, itemBytes);
    }

    std::span<const std::byte> borrowPrefix(ListView list, const OwnedShape& shape) const
    {
        const auto* owned = block(list, shape);
        if (!owned)
            return {};
        return {owned->bytes.data(), list.count * shape.words * sizeof(std::uint64_t)};
    }

    SequenceView appendSequence(SequenceView view, const OwnedShape& shape, const void* value,
                                std::size_t protectedBlocks = 0);
    std::span<const std::byte> sequenceElement(SequenceView view, const OwnedShape& shape, std::int64_t index) const;

    bool equal(const OwnedShape& shape, const std::byte* left, const std::byte* right) const
    {
        switch (shape.kind) {
        case OwnedKind::ContextOwnerReference:
            throw model::ExecutionFailure("native context owners cannot be used as collection values");
        case OwnedKind::Integer:
        case OwnedKind::UInt32:
        case OwnedKind::Boolean:
        case OwnedKind::Enum:
        case OwnedKind::Reference:
        case OwnedKind::ObjectReference:
        case OwnedKind::ResultInterface:
            return load<std::uint64_t>(left) == load<std::uint64_t>(right);
        case OwnedKind::Float:
            return std::bit_cast<double>(load<std::uint64_t>(left)) ==
                std::bit_cast<double>(load<std::uint64_t>(right));
        case OwnedKind::Float32:
            return std::bit_cast<float>(static_cast<std::uint32_t>(load<std::uint64_t>(left))) ==
                std::bit_cast<float>(static_cast<std::uint32_t>(load<std::uint64_t>(right)));
        case OwnedKind::ByteBuffer:
            return bytes().equal(load<ByteView>(left), load<ByteView>(right));
        case OwnedKind::Text:
        case OwnedKind::Error: {
            const auto a = load<TextView>(left);
            const auto b = load<TextView>(right);
            return a.size == b.size && (a.size == 0 || std::memcmp(a.bytes, b.bytes, a.size) == 0);
        }
        case OwnedKind::List: {
            const auto a = load<ListView>(left);
            const auto b = load<ListView>(right);
            if (a.count != b.count)
                return false;
            const auto& element = *shape.fields[0].shape;
            const auto* aBlock = block(a, element);
            const auto* bBlock = block(b, element);
            if (aBlock == bBlock && (a.count == 0 || reflexiveEquality(element)))
                return true;
            const auto itemBytes = element.words * sizeof(std::uint64_t);
            for (std::size_t index = 0; index < a.count; ++index) {
                if (!equal(element, aBlock->bytes.data() + index * itemBytes, bBlock->bytes.data() + index * itemBytes))
                    return false;
            }
            return true;
        }
        case OwnedKind::Array: {
            const auto a = load<ArrayView>(left);
            const auto b = load<ArrayView>(right);
            const auto& element = *shape.fields[0].shape;
            if (a.count != b.count)
                return false;
            if (a.version == b.version && (a.count == 0 || reflexiveEquality(element)))
                return true;
            for (std::size_t index = 0; index < a.count; ++index) {
                if (!equal(element, arrays_.at(a, element, index).data(), arrays_.at(b, element, index).data()))
                    return false;
            }
            return true;
        }
        case OwnedKind::Queue:
        case OwnedKind::Stack: {
            const auto a = load<SequenceView>(left);
            const auto b = load<SequenceView>(right);
            if (a.count != b.count)
                return false;
            const auto& element = *shape.fields[0].shape;
            for (std::size_t index = 0; index < a.count; ++index) {
                if (!equal(element, sequenceElement(a, shape, index).data(), sequenceElement(b, shape, index).data()))
                    return false;
            }
            return true;
        }
        case OwnedKind::HashSet:
            return sets_.equal(load<SetView>(left), load<SetView>(right), *shape.fields[0].shape);
        case OwnedKind::Dictionary:
            return dictionaries_.equal(load<DictionaryView>(left), load<DictionaryView>(right), shape);
        case OwnedKind::Optional: {
            const auto a = load<std::uint64_t>(left);
            const auto b = load<std::uint64_t>(right);
            return a == b && (a == 0 || equalField(shape.fields[1], left, right));
        }
        case OwnedKind::Result: {
            const auto a = load<ResultTag>(left);
            const auto b = load<ResultTag>(right);
            return a == b && equalField(shape.fields[1], left, right) &&
                (a != ResultTag::Success || shape.fieldCount == 2 || equalField(shape.fields[2], left, right));
        }
        case OwnedKind::Record:
        case OwnedKind::Pair:
            for (std::size_t index = 0; index < shape.fieldCount; ++index) {
                if (!equalField(shape.fields[index], left, right))
                    return false;
            }
            return true;
        case OwnedKind::FixedArray: {
            const auto& element = *shape.fields[0].shape;
            const auto stride = element.words * sizeof(std::uint64_t);
            for (std::size_t index = 0; index < shape.valueCount; ++index) {
                const auto offset = index * stride;
                if (!equal(element, left + offset, right + offset))
                    return false;
            }
            return true;
        }
        }
        throw model::ExecutionFailure("invalid native owned layout");
    }

private:
    friend class NativeDictionaries;

    struct Block {
        const OwnedShape* element;
        std::vector<std::byte> bytes;
        std::vector<model::ListOwnership> prefixes;

        model::ListOwnership costAt(std::size_t count) const
        {
            return count == 0 ? empty(*element) : prefixes[count - 1];
        }

        void append(const std::byte* input, std::size_t size, model::ListOwnership cost)
        {
            const auto oldSize = bytes.size();
            bytes.insert(bytes.end(), input, input + size);
            try {
                prefixes.push_back(cost);
            } catch (...) {
                bytes.resize(oldSize);
                throw;
            }
        }
    };

    struct ObjectLayout {
        std::size_t size;
        std::size_t alignment;

        void append(ObjectLayout field)
        {
            size = align(size, field.alignment) + field.size;
            alignment = std::max(alignment, field.alignment);
        }

        ObjectLayout finish() const
        {
            return {align(std::max(std::size_t{1}, size), alignment), alignment};
        }

        static constexpr std::size_t align(std::size_t size, std::size_t alignment)
        {
            return (size + alignment - 1) / alignment * alignment;
        }
    };

    template<class T>
    static T load(const std::byte* source)
    {
        T value;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    const Block* block(ListView view, const OwnedShape& shape) const
    {
        if (view.block == 0 && view.count == 0)
            return nullptr;
        if (view.block == 0 || view.block > blocks_.size())
            throw model::ExecutionFailure("native list does not belong to the invocation");
        const auto& owned = blocks_[view.block - 1];
        if (owned.element != &shape || view.count > owned.prefixes.size())
            throw model::ExecutionFailure("native list layout or prefix is invalid");
        return &owned;
    }

    static ObjectLayout listLayout(const OwnedShape& element)
    {
        if (element.kind == OwnedKind::Boolean)
            return {sizeof(model::List<bool>), alignof(model::List<bool>)};
        return {sizeof(model::List<std::int64_t>), alignof(model::List<std::int64_t>)};
    }

    static model::ListOwnership empty(const OwnedShape& element) { return {listLayout(element).size, 0}; }

    bool equalField(const OwnedField& field, const std::byte* left, const std::byte* right) const
    {
        const auto offset = field.offsetWords * sizeof(std::uint64_t);
        return equal(*field.shape, left + offset, right + offset);
    }

    static ObjectLayout optionalLayout(const OwnedShape& shape)
    {
        // The supported standard libraries place the optional flag after
        // its value. Verify representative alignments at compile time.
        static_assert(sizeof(std::optional<bool>) == 2);
        static_assert(sizeof(std::optional<std::int64_t>) == sizeof(std::int64_t) * 2);
        static_assert(sizeof(std::optional<std::string>) == sizeof(std::string) + alignof(std::string));
        auto value = referenceLayout(shape);
        value.size += value.alignment;
        return value;
    }

    static ObjectLayout referenceLayout(const OwnedShape& shape)
    {
        switch (shape.kind) {
        case OwnedKind::ContextOwnerReference:
            throw model::ExecutionFailure("native context owners cannot be used as collection values");
        case OwnedKind::Integer: return {sizeof(std::int64_t), alignof(std::int64_t)};
        case OwnedKind::UInt32: return {sizeof(std::uint32_t), alignof(std::uint32_t)};
        case OwnedKind::Float: return {sizeof(double), alignof(double)};
        case OwnedKind::Float32: return {sizeof(float), alignof(float)};
        case OwnedKind::ByteBuffer: return {sizeof(model::ByteBuffer), alignof(model::ByteBuffer)};
        case OwnedKind::Boolean: return {sizeof(bool), alignof(bool)};
        case OwnedKind::Enum: return {sizeof(int), alignof(int)};
        case OwnedKind::Reference:
        case OwnedKind::ObjectReference:
        case OwnedKind::ResultInterface: return {2 * sizeof(void*), alignof(void*)};
        case OwnedKind::Text: return {sizeof(std::string), alignof(std::string)};
        case OwnedKind::Error: return {sizeof(model::Error), alignof(model::Error)};
        case OwnedKind::List: return listLayout(*shape.fields[0].shape);
        case OwnedKind::Array: return {sizeof(model::Array<std::int64_t>), alignof(model::Array<std::int64_t>)};
        case OwnedKind::Queue: return {sizeof(model::Queue<std::int64_t>), alignof(model::Queue<std::int64_t>)};
        case OwnedKind::Stack: return {sizeof(model::Stack<std::int64_t>), alignof(model::Stack<std::int64_t>)};
        case OwnedKind::HashSet:
            return {sizeof(model::HashSet<std::int64_t>), alignof(model::HashSet<std::int64_t>)};
        case OwnedKind::Dictionary:
            return {sizeof(model::Dictionary<std::int64_t, std::int64_t>),
                    alignof(model::Dictionary<std::int64_t, std::int64_t>)};
        case OwnedKind::Optional: return optionalLayout(*shape.fields[1].shape);
        case OwnedKind::FixedArray: {
            if (shape.valueCount == 0)
                return {1, 1};
            auto element = referenceLayout(*shape.fields[0].shape);
            element.size *= shape.valueCount;
            return element;
        }
        case OwnedKind::Pair:
        case OwnedKind::Record: {
            ObjectLayout record{0, 1};
            for (std::size_t index = 0; index < shape.fieldCount; ++index)
                record.append(referenceLayout(*shape.fields[index].shape));
            return record.finish();
        }
        case OwnedKind::Result: {
            ObjectLayout result{sizeof(model::Outcome), alignof(model::Outcome)};
            if (shape.fieldCount == 3)
                result.append(optionalLayout(*shape.fields[2].shape));
            result.append(referenceLayout(*shape.fields[1].shape));
            return result.finish();
        }
        }
        throw model::ExecutionFailure("invalid native owned layout");
    }

    static bool reflexiveEquality(const OwnedShape& shape)
    {
        if (shape.kind == OwnedKind::Float || shape.kind == OwnedKind::Float32)
            return false;
        for (std::size_t field = 0; field < shape.fieldCount; ++field) {
            if (!reflexiveEquality(*shape.fields[field].shape))
                return false;
        }
        return true;
    }

    model::ListOwnership cost(const OwnedShape& shape, const std::byte* value) const
    {
        if (shape.kind == OwnedKind::List) {
            const auto list = load<ListView>(value);
            const auto& element = *shape.fields[0].shape;
            const auto* owned = block(list, element);
            return owned ? owned->costAt(list.count) : empty(element);
        }
        if (shape.kind == OwnedKind::HashSet)
            return sets_.ownership(load<SetView>(value), *shape.fields[0].shape);
        if (shape.kind == OwnedKind::Dictionary)
            return dictionaries_.ownership(load<DictionaryView>(value), shape);
        if (shape.kind == OwnedKind::Queue || shape.kind == OwnedKind::Stack)
            return sequenceCost(load<SequenceView>(value), shape);
        if (shape.kind == OwnedKind::Array) {
            const auto total = arrays_.ownership(load<ArrayView>(value), *shape.fields[0].shape);
            return {total.bytes, total.elements};
        }

        if (shape.kind == OwnedKind::ResultInterface) {
            const auto opaque = load<ResultInterfaceHeader>(value);
            if (opaque.identity == 0)
                throw model::ExecutionFailure("result interface is empty");
            return {opaque.ownedBytes, opaque.ownedElements};
        }

        model::ListOwnership result{referenceLayout(shape).size, 0};
        if (shape.kind == OwnedKind::Text || shape.kind == OwnedKind::Error) {
            const auto text = load<TextView>(value);
            if (shape.kind == OwnedKind::Error)
                result.bytes = model::addOwnedBytes(result.bytes, sizeof(std::string));
            result.bytes = model::addOwnedBytes(result.bytes, text.size);
        } else if (shape.kind == OwnedKind::ByteBuffer) {
            result.bytes = model::addOwnedBytes(result.bytes, bytes().view(load<ByteView>(value)).size());
        } else if (shape.kind == OwnedKind::Optional) {
            if (load<std::uint64_t>(value) != 0) {
                const auto& field = shape.fields[1];
                const auto owned = cost(*field.shape, value + field.offsetWords * sizeof(std::uint64_t));
                result.bytes = model::addOwnedBytes(result.bytes, owned.bytes);
                result.elements = owned.elements;
            }
        } else if (shape.kind == OwnedKind::FixedArray) {
            const auto& element = *shape.fields[0].shape;
            const auto stride = element.words * sizeof(std::uint64_t);
            result.elements = shape.valueCount;
            for (std::size_t index = 0; index < shape.valueCount; ++index) {
                const auto owned = cost(element, value + index * stride);
                result.bytes = model::addOwnedBytes(result.bytes, owned.bytes);
                result.elements = model::boundedOwnedSum(result.elements, owned.elements, model::maximumListElements);
            }
        } else if (shape.kind == OwnedKind::Record || shape.kind == OwnedKind::Pair || shape.kind == OwnedKind::Result) {
            const auto tag = shape.kind == OwnedKind::Result ? load<ResultTag>(value) : ResultTag::Success;
            const auto firstField = shape.kind == OwnedKind::Result ? 1U : 0U;
            const auto end = shape.kind == OwnedKind::Result && tag != ResultTag::Success ? 2 : shape.fieldCount;
            for (std::size_t index = firstField; index < end; ++index) {
                const auto& field = shape.fields[index];
                const auto owned = cost(*field.shape, value + field.offsetWords * sizeof(std::uint64_t));
                result.bytes = model::addOwnedBytes(result.bytes, owned.bytes);
                result.elements = model::boundedOwnedSum(result.elements, owned.elements, model::maximumListElements);
            }
        }
        return result;
    }

    ListView sequencePrefix(SequenceView view, const OwnedShape& element) const;
    model::ListOwnership sequenceCost(SequenceView view, const OwnedShape& shape) const;

    NativeArrays::Ownership arrayItemCost(const OwnedShape& element, const void* value) const
    {
        const auto item = cost(element, static_cast<const std::byte*>(value));
        return {item.bytes, model::boundedOwnedSum(1, item.elements, model::maximumListElements)};
    }

    void admitArena(std::size_t growth) const
    {
        constexpr auto maximumArenaBytes = 32 * model::maximumListBytes;
        if (growth > maximumArenaBytes - bytes_)
            throw model::EvaluationLimitFailure("native invocation list storage limit exceeded");
    }

    std::vector<Block> blocks_;
    NativeArrays arrays_;
    NativeSets sets_;
    NativeDictionaries dictionaries_;
    NativeBytes byteBuffers_;
    std::size_t bytes_ = 0;
};

}
