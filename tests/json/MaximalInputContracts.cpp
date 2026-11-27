// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

int main() {
    constexpr std::size_t values = 2097151;
    std::string input;
    input.reserve(4194304);
    input += '[';
    for (std::size_t index = 0; index < values; ++index) {
        if (index != 0)
            input += ',';
        input += '0';
    }
    input += "] ";
    if (input.size() != 4194304)
        throw std::runtime_error("maximal fixture size differs");

    pipelang::json::Reader reader;
    pipelang::json::ReadLimits limits;
    limits.nodes = 2097152;
    limits.retainedBytes = 512 * 1024 * 1024;
    const auto document = reader.begin(limits);
    for (std::size_t offset = 0; offset < input.size(); offset += 65536) {
        const auto count = std::min<std::size_t>(65536, input.size() - offset);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data() + offset);
        reader.append(document, std::span<const std::uint8_t>(bytes, count));
    }

    const auto root = reader.complete(document);
    if (reader.count(root) != static_cast<std::int64_t>(values) ||
        reader.integer(reader.at(root, values - 1)) != 0)
        throw std::runtime_error("maximal flat array traversal differs");

    reader.close(document);
    if (reader.liveDocuments() != 0)
        throw std::runtime_error("maximal flat array retained its document");

    std::cout << "4194304 input bytes; 2097152 nodes; exact final value and cleanup passed\n";
}
