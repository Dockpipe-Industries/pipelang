// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pipelang::model {

inline constexpr std::size_t maximumByteBufferBytes = 1024 * 1024;

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
        const auto position = checkedOffset(index);
        if (position == bytes_.size())
            throw ExecutionFailure("byte buffer index out of range");
        return bytes_[position];
    }

    ByteBuffer slice(std::int64_t start, std::int64_t count) const {
        const auto position = checkedOffset(start);
        if (count < 0 || static_cast<std::uint64_t>(count) > bytes_.size() - position)
            throw ExecutionFailure("byte buffer count out of range");
        const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(position);
        const auto last = first + static_cast<std::ptrdiff_t>(count);
        return ByteBuffer(std::vector<std::uint8_t>(first, last));
    }

    std::int64_t indexOf(const ByteBuffer& pattern, std::int64_t start) const {
        const auto position = checkedOffset(start);
        if (pattern.bytes_.empty())
            return start;
        if (pattern.bytes_.size() > bytes_.size() - position)
            return -1;
        // Prefix matching keeps repeated-byte inputs linear in both lengths.
        std::vector<std::size_t> prefix(pattern.bytes_.size(), 0);
        std::size_t matched = 0;
        for (std::size_t index = 1; index < pattern.bytes_.size(); ++index) {
            while (matched > 0 && pattern.bytes_[index] != pattern.bytes_[matched])
                matched = prefix[matched - 1];
            if (pattern.bytes_[index] == pattern.bytes_[matched])
                ++matched;
            prefix[index] = matched;
        }
        matched = 0;
        for (std::size_t index = position; index < bytes_.size(); ++index) {
            while (matched > 0 && bytes_[index] != pattern.bytes_[matched])
                matched = prefix[matched - 1];
            if (bytes_[index] == pattern.bytes_[matched])
                ++matched;
            if (matched == pattern.bytes_.size())
                return static_cast<std::int64_t>(index + 1 - matched);
        }
        return -1;
    }

    void append(ByteBuffer value) {
        if (value.bytes_.size() > maximumByteBufferBytes - bytes_.size())
            throw ExecutionFailure("byte buffer limit exceeded");
        bytes_.insert(bytes_.end(), value.bytes_.begin(), value.bytes_.end());
    }

    void add(std::uint32_t value) {
        if (value > 255)
            throw ExecutionFailure("byte buffer value must be between 0 and 255");
        if (bytes_.size() == maximumByteBufferBytes)
            throw ExecutionFailure("byte buffer limit exceeded");
        bytes_.push_back(static_cast<std::uint8_t>(value));
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
    std::size_t checkedOffset(std::int64_t offset) const {
        if (offset < 0 || static_cast<std::uint64_t>(offset) > bytes_.size())
            throw ExecutionFailure("byte buffer offset out of range");
        return static_cast<std::size_t>(offset);
    }

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
