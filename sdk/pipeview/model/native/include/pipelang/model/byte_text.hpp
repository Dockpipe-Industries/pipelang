// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace pipelang::model {

// Byte-oriented operations deliberately do not decode Unicode or stop at NUL.
// Comparing sizes avoids narrowing a native size_t to the language's signed64.
inline bool byteLengthIs(std::string_view value, std::int64_t expected) noexcept
{
    return expected >= 0 && std::cmp_equal(value.size(), expected);
}

inline std::int64_t byteAtOr(std::string_view value, std::int64_t index,
                             std::int64_t fallback) noexcept
{
    if (index < 0 || std::cmp_greater_equal(index, value.size()))
        return fallback;
    return static_cast<unsigned char>(value[static_cast<std::size_t>(index)]);
}

}
