// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace pipelang::json;

void check(bool matched, const char* reason) {
    if (!matched)
        throw std::runtime_error(reason);
}

void append(Reader& reader, std::int64_t document, std::span<const std::uint8_t> input) {
    while (!input.empty()) {
        const auto count = std::min(input.size(), std::size_t{4096});
        reader.append(document, input.first(count));
        input = input.subspan(count);
    }
}

void codeUnit(std::vector<std::uint8_t>& bytes, std::uint32_t value, unsigned width, bool bigEndian) {
    for (unsigned index = 0; index < width; ++index) {
        const unsigned offset = bigEndian ? width - index - 1 : index;
        bytes.push_back(static_cast<std::uint8_t>(value >> (offset * 8U)));
    }
}

std::vector<std::uint8_t> encode(std::u32string_view input, unsigned width, bool bigEndian, bool bom) {
    std::vector<std::uint8_t> output;
    if (bom)
        codeUnit(output, 0xfeff, width, bigEndian);

    for (char32_t scalar : input) {
        if (width == 2 && scalar > 0xffff) {
            scalar -= 0x10000;
            codeUnit(output, 0xd800U | (scalar >> 10U), width, bigEndian);
            codeUnit(output, 0xdc00U | (scalar & 0x3ffU), width, bigEndian);
        } else {
            codeUnit(output, scalar, width, bigEndian);
        }
    }

    return output;
}

std::int64_t read(Reader& reader, std::span<const std::uint8_t> input, bool wide) {
    ReadLimits limits;
    limits.inputBytes = 32768;
    limits.retainedBytes = 524288;
    limits.detectUnicodeEncoding = wide;
    const auto document = reader.begin(limits);
    append(reader, document, input);
    return document;
}

void decoded(Reader& reader, const std::vector<std::uint8_t>& input) {
    const auto document = read(reader, input, true);
    const auto root = reader.complete(document);
    const auto node = reader.find(root, "value");
    const std::vector<std::uint8_t> expected{0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80};
    check(node.has_value() && reader.scalarLength(*node) == 7 &&
          reader.scalarSlice(*node, 0, 7) == expected, "decoded Unicode scalar values changed");
    const auto size = static_cast<std::int64_t>(input.size());
    check(reader.inputLength(document) == size && reader.inputSlice(document, 0, size) == input,
          "captured encoded input bytes changed");

    reader.close(document);
    check(Reader::aggregateRetainedBytes() == 0, "normalization arena remained allocated");
}

void rejected(Reader& reader, std::span<const std::uint8_t> input, bool wide, std::string_view expected,
              const std::function<bool()>& cancelled = {}) {
    const auto document = read(reader, input, wide);
    try {
        reader.complete(document, cancelled);
        throw std::logic_error("expected a candidate input refusal");
    } catch (const ReadFailure& error) {
        check(std::string_view(error.what()) == expected, "incorrect encoded input refusal");
    }
    check(reader.inputLength(document) == static_cast<std::int64_t>(input.size()),
          "failed normalization lost the original capture");
    reader.close(document);
    check(Reader::aggregateRetainedBytes() == 0, "failed normalization retained storage");
}

void varieties() {
    Reader reader;
    const std::vector<std::uint8_t> utf8{0xef, 0xbb, 0xbf, '{', '"', 'v', 'a', 'l', 'u', 'e', '"',
        ':', '"', 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80, '"', '}'};
    decoded(reader, utf8);
    rejected(reader, utf8, false, "json.encoding");

    constexpr std::u32string_view fixture = U"{\"value\":\"\u20ac\U0001f600\"}";
    for (unsigned width : {2U, 4U}) {
        for (bool bigEndian : {false, true}) {
            for (bool bom : {false, true})
                decoded(reader, encode(fixture, width, bigEndian, bom));
        }
    }

    const auto primitive = encode(U"7", 2, false, false);
    const auto document = read(reader, primitive, true);
    check(reader.integer(reader.complete(document)) == 7, "two-byte UTF-16 heuristic changed");
    reader.close(document);
}

void failures() {
    Reader reader;
    rejected(reader, std::array<std::uint8_t, 4>{0xff, 0xfe, '{', 0}, false, "json.syntax");
    rejected(reader, std::array<std::uint8_t, 5>{0xff, 0xfe, '{', 0, '}'}, true, "json.encoding");
    rejected(reader, std::array<std::uint8_t, 4>{0xff, 0xfe, 0, 0xd8}, true, "json.encoding");
    rejected(reader, std::array<std::uint8_t, 8>{0, 0, 0xfe, 0xff, 0, 0x11, 0, 0}, true, "json.encoding");
    rejected(reader, std::array<std::uint8_t, 7>{0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf, '1'}, true,
             "json.encoding");
    const auto trailingNul = std::vector<std::uint8_t>{0xff, 0xfe, '7', 0, 0, 0, '8', 0};
    rejected(reader, trailingNul, true, "json.syntax");

    std::vector<std::uint8_t> large = encode(U"7", 2, true, true);
    for (std::size_t index = 0; index < 8192; ++index)
        codeUnit(large, 0x20, 2, true);

    int checks = 0;
    rejected(reader, large, true, "json.cancelled", [&] { return ++checks == 3; });
    check(checks == 3, "wide decoder did not observe cancellation during work");
}

} // namespace

int main() {
    try {
        varieties();
        failures();
        std::cout << "Optional JSON wire encodings, strict defaults and exact captured bytes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
