// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>

namespace pipelang::json::detail {

struct InputEncoding {
    std::size_t unitBytes = 1;
    std::size_t prefixBytes = 0;
    bool bigEndian = false;
};

inline bool startsWith(std::span<const std::uint8_t> input,
                       std::initializer_list<std::uint8_t> prefix) {
    return input.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), input.begin());
}

inline bool hasUtf8Bom(std::span<const std::uint8_t> input) {
    return startsWith(input, {0xef, 0xbb, 0xbf});
}

inline InputEncoding detectInputEncoding(std::span<const std::uint8_t> input) {
    if (startsWith(input, {0, 0, 0xfe, 0xff}))
        return {4, 4, true};
    if (startsWith(input, {0xff, 0xfe, 0, 0}))
        return {4, 4, false};
    if (startsWith(input, {0xfe, 0xff}))
        return {2, 2, true};
    if (startsWith(input, {0xff, 0xfe}))
        return {2, 2, false};
    if (hasUtf8Bom(input))
        return {1, 3, false};

    // A JSON document begins with an ASCII structural character, primitive,
    // or whitespace. The initial code-unit padding distinguishes wide forms
    // when no BOM is present.
    if (input.size() >= 4) {
        if (input[0] == 0)
            return input[1] == 0 ? InputEncoding{4, 0, true} : InputEncoding{2, 0, true};

        if (input[1] == 0)
            return input[2] == 0 && input[3] == 0 ? InputEncoding{4, 0, false} : InputEncoding{2, 0, false};
    } else if (input.size() == 2) {
        if (input[0] == 0)
            return {2, 0, true};
        if (input[1] == 0)
            return {2, 0, false};
    }

    return {};
}

inline std::uint32_t codeUnit(std::span<const std::uint8_t> bytes, bool bigEndian) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto selected = bigEndian ? index : bytes.size() - index - 1;
        result = (result << 8U) | bytes[selected];
    }

    return result;
}

inline std::size_t utf8Scalar(std::uint32_t scalar, std::span<std::uint8_t> output) {
    if (scalar <= 0x7f) {
        output[0] = static_cast<std::uint8_t>(scalar);
        return 1;
    }
    if (scalar <= 0x7ff) {
        output[0] = static_cast<std::uint8_t>(0xc0U | (scalar >> 6U));
        output[1] = static_cast<std::uint8_t>(0x80U | (scalar & 0x3fU));
        return 2;
    }
    if (scalar <= 0xffff) {
        output[0] = static_cast<std::uint8_t>(0xe0U | (scalar >> 12U));
        output[1] = static_cast<std::uint8_t>(0x80U | ((scalar >> 6U) & 0x3fU));
        output[2] = static_cast<std::uint8_t>(0x80U | (scalar & 0x3fU));
        return 3;
    }

    output[0] = static_cast<std::uint8_t>(0xf0U | (scalar >> 18U));
    output[1] = static_cast<std::uint8_t>(0x80U | ((scalar >> 12U) & 0x3fU));
    output[2] = static_cast<std::uint8_t>(0x80U | ((scalar >> 6U) & 0x3fU));
    output[3] = static_cast<std::uint8_t>(0x80U | (scalar & 0x3fU));
    return 4;
}

inline std::size_t decodeWideInput(std::span<const std::uint8_t> input, InputEncoding encoding,
                                   std::span<std::uint8_t> output,
                                   const std::function<bool()>& cancelled) {
    if (input.size() % encoding.unitBytes != 0)
        throw ReadFailure("json.encoding");

    std::size_t produced = 0;
    std::size_t nextCheckpoint = 0;
    for (std::size_t consumed = 0; consumed < input.size(); consumed += encoding.unitBytes) {
        if (consumed >= nextCheckpoint) {
            if (cancelled && cancelled())
                throw ReadFailure("json.cancelled");

            nextCheckpoint = consumed + 4096;
        }

        auto scalar = codeUnit(input.subspan(consumed, encoding.unitBytes), encoding.bigEndian);
        if (encoding.unitBytes == 2 && scalar >= 0xd800 && scalar <= 0xdbff) {
            consumed += encoding.unitBytes;
            if (consumed >= input.size())
                throw ReadFailure("json.encoding");

            const auto low = codeUnit(input.subspan(consumed, encoding.unitBytes), encoding.bigEndian);
            if (low < 0xdc00 || low > 0xdfff)
                throw ReadFailure("json.encoding");

            scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
        }
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            throw ReadFailure("json.encoding");

        produced += utf8Scalar(scalar, output.subspan(produced));
    }

    return produced;
}

} // namespace pipelang::json::detail
