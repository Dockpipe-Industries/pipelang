// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace pipelang::native_app_v19 {

class NativeBytes {
public:
    static ByteView borrowed(std::string_view bytes);
    static std::string_view asText(std::span<const std::uint8_t> bytes);

    std::span<const std::uint8_t> view(ByteView value) const;
    std::uint8_t at(ByteView receiver, std::int64_t index) const;
    std::int64_t indexOf(ByteView receiver, ByteView value, std::int64_t start) const;
    bool equal(ByteView left, ByteView right) const;
    ByteView slice(ByteView source, std::int64_t start, std::int64_t count) const;

    std::size_t checkpoint() const noexcept { return blocks_.size(); }
    void restore(std::size_t checkpoint) noexcept;

    ByteView append(ByteView receiver, ByteView value, std::size_t protectedBlocks);
    ByteView add(ByteView receiver, std::uint32_t value, std::size_t protectedBlocks);

private:
    using Block = std::vector<std::uint8_t>;

    void admit(std::size_t bytes) const;
    void reserve(Block& block, std::size_t size);
    ByteView extend(ByteView receiver, std::span<const std::uint8_t> suffix, ByteView alias,
                    std::size_t protectedBlocks);

    std::vector<Block> blocks_;
    std::size_t bytes_ = 0;
};

}
