// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeBytes.hpp"

#include <pipelang/model/byte_buffer.hpp>

#include <algorithm>

namespace pipelang::native_app_v19 {

namespace {

constexpr auto maximumBytes = 32 * model::maximumByteBufferBytes;

}

ByteView NativeBytes::borrowed(std::string_view bytes)
{
    return bytes.empty() ? ByteView{} : ByteView{0, reinterpret_cast<std::uint64_t>(bytes.data()), bytes.size()};
}

std::string_view NativeBytes::asText(std::span<const std::uint8_t> bytes)
{
    return bytes.empty() ? std::string_view{} :
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::span<const std::uint8_t> NativeBytes::view(ByteView value) const
{
    if (value.size > model::maximumByteBufferBytes)
        throw model::ExecutionFailure("invalid native byte buffer size");
    if (value.block == 0) {
        if (value.size != 0 && value.offset == 0)
            throw model::ExecutionFailure("invalid native byte buffer borrow");
        return {reinterpret_cast<const std::uint8_t*>(value.offset), static_cast<std::size_t>(value.size)};
    }
    if (value.block > blocks_.size())
        throw model::ExecutionFailure("invalid native byte buffer handle");
    const auto& block = blocks_[value.block - 1];
    if (value.offset > block.size() || value.size > block.size() - value.offset)
        throw model::ExecutionFailure("invalid native byte buffer range");
    return std::span(block).subspan(value.offset, value.size);
}

std::uint8_t NativeBytes::at(ByteView receiver, std::int64_t index) const
{
    return model::byte_buffer::at(view(receiver), index);
}

std::int64_t NativeBytes::indexOf(ByteView receiver, ByteView value, std::int64_t start) const
{
    return model::byte_buffer::indexOf(view(receiver), view(value), start);
}

bool NativeBytes::equal(ByteView left, ByteView right) const
{
    const auto first = view(left);
    const auto second = view(right);
    return std::equal(first.begin(), first.end(), second.begin(), second.end());
}

ByteView NativeBytes::slice(ByteView source, std::int64_t start, std::int64_t count) const
{
    const auto bytes = model::byte_buffer::slice(view(source), start, count);
    if (source.block == 0)
        return borrowed(asText(bytes));
    return {source.block, source.offset + static_cast<std::uint64_t>(start), bytes.size()};
}

void NativeBytes::restore(std::size_t checkpoint) noexcept
{
    while (blocks_.size() > checkpoint) {
        bytes_ -= blocks_.back().capacity();
        blocks_.pop_back();
    }
}

ByteView NativeBytes::append(ByteView receiver, ByteView value, std::size_t protectedBlocks)
{
    return extend(receiver, view(value), value, protectedBlocks);
}

ByteView NativeBytes::add(ByteView receiver, std::uint32_t value, std::size_t protectedBlocks)
{
    const auto byte = model::byte_buffer::byte(value);
    return extend(receiver, std::span(&byte, 1), {}, protectedBlocks);
}

void NativeBytes::admit(std::size_t bytes) const
{
    if (bytes > maximumBytes - bytes_)
        throw model::EvaluationLimitFailure("native invocation byte buffer storage limit exceeded");
}

void NativeBytes::reserve(Block& block, std::size_t size)
{
    if (size <= block.capacity())
        return;
    const auto growth = std::min(model::maximumByteBufferBytes, block.capacity() * 2);
    const auto capacity = std::max(size, growth);
    admit(capacity - block.capacity());
    Block replacement;
    replacement.reserve(capacity);
    const auto added = replacement.capacity() - block.capacity();
    admit(added);
    replacement.insert(replacement.end(), block.begin(), block.end());
    block.swap(replacement);
    bytes_ += added;
}

ByteView NativeBytes::extend(ByteView receiver, std::span<const std::uint8_t> suffix, ByteView alias,
                             std::size_t protectedBlocks)
{
    const auto prefix = view(receiver);
    model::byte_buffer::admitAppend(prefix.size(), suffix.size());
    if (suffix.empty())
        return receiver;
    const auto size = prefix.size() + suffix.size();

    // A constructor may grow a previously allocated prefix only in a new block.
    // Its savepoint can then release the entire growth on failure.
    if (receiver.block > protectedBlocks && receiver.offset == 0 &&
        prefix.size() == blocks_[receiver.block - 1].size()) {
        // Older views keep their lengths. A self-append recomputes the
        // source address after growing storage, then copies from the prefix.
        auto& block = blocks_[receiver.block - 1];
        reserve(block, size);
        if (alias.block == receiver.block)
            suffix = std::span<const std::uint8_t>(block).subspan(alias.offset, alias.size);
        const auto start = block.size();
        block.resize(size);
        std::copy(suffix.begin(), suffix.end(), block.begin() + static_cast<std::ptrdiff_t>(start));
        return {receiver.block, 0, size};
    }

    if (blocks_.size() >= model::maximumListElements)
        throw model::EvaluationLimitFailure("native invocation byte buffer block limit exceeded");
    admit(size);
    Block block;
    block.reserve(size);
    admit(block.capacity());
    block.insert(block.end(), prefix.begin(), prefix.end());
    block.insert(block.end(), suffix.begin(), suffix.end());
    const auto capacity = block.capacity();
    blocks_.push_back(std::move(block));
    bytes_ += capacity;
    return {blocks_.size(), 0, size};
}

}
