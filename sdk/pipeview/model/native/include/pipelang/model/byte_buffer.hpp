// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pipelang::model {

inline constexpr std::size_t maximumByteBufferBytes = 1024 * 1024;

namespace byte_buffer {

inline std::size_t offset(std::size_t size, std::int64_t value) {
    if (value < 0 || static_cast<std::uint64_t>(value) > size)
        throw ExecutionFailure("byte buffer offset out of range");
    return static_cast<std::size_t>(value);
}

inline std::uint8_t at(std::span<const std::uint8_t> bytes, std::int64_t index) {
    const auto position = offset(bytes.size(), index);
    if (position == bytes.size())
        throw ExecutionFailure("byte buffer index out of range");
    return bytes[position];
}

inline std::span<const std::uint8_t> slice(std::span<const std::uint8_t> bytes, std::int64_t start, std::int64_t count) {
    const auto position = offset(bytes.size(), start);
    if (count < 0 || static_cast<std::uint64_t>(count) > bytes.size() - position)
        throw ExecutionFailure("byte buffer count out of range");
    return bytes.subspan(position, static_cast<std::size_t>(count));
}

inline std::int64_t indexOf(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> pattern,
                            std::int64_t start) {
    const auto position = offset(bytes.size(), start);
    if (pattern.empty())
        return start;
    if (pattern.size() > bytes.size() - position)
        return -1;
    // Prefix matching keeps repeated-byte inputs linear in both lengths.
    std::vector<std::size_t> prefix(pattern.size(), 0);
    std::size_t matched = 0;
    for (std::size_t index = 1; index < pattern.size(); ++index) {
        while (matched > 0 && pattern[index] != pattern[matched])
            matched = prefix[matched - 1];
        if (pattern[index] == pattern[matched])
            ++matched;
        prefix[index] = matched;
    }
    matched = 0;
    for (std::size_t index = position; index < bytes.size(); ++index) {
        while (matched > 0 && bytes[index] != pattern[matched])
            matched = prefix[matched - 1];
        if (bytes[index] == pattern[matched])
            ++matched;
        if (matched == pattern.size())
            return static_cast<std::int64_t>(index + 1 - matched);
    }
    return -1;
}

inline void admitAppend(std::size_t size, std::size_t count) {
    if (size > maximumByteBufferBytes || count > maximumByteBufferBytes - size)
        throw ExecutionFailure("byte buffer limit exceeded");
}

inline std::uint8_t byte(std::uint32_t value) {
    if (value > 255)
        throw ExecutionFailure("byte buffer value must be between 0 and 255");
    return static_cast<std::uint8_t>(value);
}

}

class ByteBuffer {
public:
    ByteBuffer() = default;

    explicit ByteBuffer(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
        if (bytes_.size() > maximumByteBufferBytes)
            throw ExecutionFailure("byte buffer limit exceeded");
    }

    static ByteBuffer Empty() {
        return {};
    }

    static ByteBuffer FromUtf8(const std::string& text) {
        validate(text);
        return ByteBuffer(std::vector<std::uint8_t>(text.begin(), text.end()));
    }

    std::int64_t length() const {
        return static_cast<std::int64_t>(bytes_.size());
    }

    std::uint32_t at(std::int64_t index) const {
        return byte_buffer::at(bytes_, index);
    }

    ByteBuffer slice(std::int64_t start, std::int64_t count) const {
        const auto slice = byte_buffer::slice(bytes_, start, count);
        return ByteBuffer(std::vector<std::uint8_t>(slice.begin(), slice.end()));
    }

    std::int64_t indexOf(const ByteBuffer& pattern, std::int64_t start) const {
        return byte_buffer::indexOf(bytes_, pattern.bytes_, start);
    }

    void append(ByteBuffer value) {
        byte_buffer::admitAppend(bytes_.size(), value.bytes_.size());
        bytes_.insert(bytes_.end(), value.bytes_.begin(), value.bytes_.end());
    }

    void add(std::uint32_t value) {
        const auto admitted = byte_buffer::byte(value);
        byte_buffer::admitAppend(bytes_.size(), 1);
        bytes_.push_back(admitted);
    }

    std::string decodeUtf8() const {
        std::string text(bytes_.begin(), bytes_.end());
        validate(text);
        return text;
    }

    const std::vector<std::uint8_t>& values() const {
        return bytes_;
    }

    bool operator==(const ByteBuffer&) const = default;

private:
    std::vector<std::uint8_t> bytes_;
};

inline void validate(const ByteBuffer& value) {
    if (value.values().size() > maximumByteBufferBytes)
        throw ExecutionFailure("byte buffer limit exceeded");
}

inline std::size_t ownedBytes(const ByteBuffer& value) {
    return sizeof(ByteBuffer) + value.values().size();
}

inline std::size_t ownedElements(const ByteBuffer&) {
    return 0;
}

}
