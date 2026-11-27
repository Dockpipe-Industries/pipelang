// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/json/Reader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace pipelang::json {

namespace detail {

// A renderer owns only a bounded staging fragment. Already delivered fragments
// belong to the caller's sink and are retained when a later operation fails.
class ValueRenderer {
public:
    ValueRenderer(RenderOptions options, const JsonByteSink& sink,
                  const std::function<bool()>& cancelled)
        : options_(options), sink_(sink), cancelled_(cancelled) {}

    void checkpoint() const {
        if (cancelled_ && cancelled_())
            throw ReadFailure("json.cancelled");
    }

    std::size_t finish() {
        flush();
        checkpoint();
        return size_;
    }

    template<class Nodes, class Node>
    void write(const Nodes& nodes, const Node& node, std::size_t depth) {
        checkpoint();
        switch (node.kind) {
        case Kind::Null:
            append("null");
            return;
        case Kind::Boolean:
            append(node.boolean ? "true" : "false");
            return;
        case Kind::Number:
            append(node.text);
            return;
        case Kind::String:
            quoted(node.text);
            return;
        case Kind::Array:
            append("[");
            array(nodes, node, depth);
            append("]");
            return;
        case Kind::Object:
            append("{");
            object(nodes, node, depth);
            append("}");
            return;
        }
    }

private:
    RenderOptions options_;
    const JsonByteSink& sink_;
    const std::function<bool()>& cancelled_;
    std::array<std::uint8_t, 4096> buffer_{};
    std::size_t used_ = 0;
    std::size_t size_ = 0;

    void flush() {
        checkpoint();
        if (used_ == 0)
            return;

        size_ += used_;
        sink_(std::span<const std::uint8_t>(buffer_.data(), used_));
        used_ = 0;
    }

    void append(std::string_view text) {
        if (text.size() > options_.maximumBytes - size_ - used_)
            throw ReadFailure("json.render.byte_limit");

        while (!text.empty()) {
            const auto available = std::min(buffer_.size() - used_, text.size());
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
            std::copy_n(bytes, available, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += available;
            text.remove_prefix(available);
            if (used_ == buffer_.size())
                flush();
        }
    }

    void whitespace(std::size_t depth) {
        if (options_.layout != RenderLayout::Indented)
            return;

        append("\n");
        constexpr std::string_view spaces = "                                ";
        std::size_t remaining = depth * 2;
        while (remaining != 0) {
            const auto count = std::min(remaining, spaces.size());
            append(spaces.substr(0, count));
            remaining -= count;
        }
    }

    void escapedWord(std::uint32_t code) {
        constexpr char hex[] = "0123456789abcdef";
        const std::array<char, 6> word{'\\', 'u', hex[(code >> 12U) & 15U],
            hex[(code >> 8U) & 15U], hex[(code >> 4U) & 15U], hex[code & 15U]};
        append(std::string_view(word.data(), word.size()));
    }

    // Reader storage contains validated UTF-8. Consume exactly one non-ASCII
    // scalar and encode supplemental planes with a UTF-16 surrogate pair.
    void escapedScalar(std::string_view text, std::size_t& offset) {
        const auto leading = static_cast<std::uint8_t>(text[offset]);
        std::size_t length = 2;
        std::uint32_t code = leading & 31U;
        if (leading >= 240U) {
            length = 4;
            code = leading & 7U;
        } else if (leading >= 224U) {
            length = 3;
            code = leading & 15U;
        }

        for (std::size_t part = 1; part < length; ++part)
            code = (code << 6U) | (static_cast<std::uint8_t>(text[offset + part]) & 63U);

        offset += length - 1;
        if (code <= 0xffffU) {
            escapedWord(code);
            return;
        }

        code -= 0x10000U;
        escapedWord(0xd800U | (code >> 10U));
        escapedWord(0xdc00U | (code & 0x3ffU));
    }

    void quoted(std::string_view text) {
        append("\"");
        for (std::size_t offset = 0; offset < text.size(); ++offset) {
            const auto byte = static_cast<std::uint8_t>(text[offset]);
            switch (byte) {
            case '"':
                append("\\\"");
                break;
            case '\\':
                append("\\\\");
                break;
            case '\b':
                append("\\b");
                break;
            case '\f':
                append("\\f");
                break;
            case '\n':
                append("\\n");
                break;
            case '\r':
                append("\\r");
                break;
            case '\t':
                append("\\t");
                break;
            default:
                if (byte < 32U || (options_.ascii && byte == 127U)) {
                    escapedWord(byte);
                } else if (options_.ascii && byte > 127U) {
                    escapedScalar(text, offset);
                } else {
                    append(text.substr(offset, 1));
                }
            }
        }
        append("\"");
    }

    template<class Nodes, class Node>
    void array(const Nodes& nodes, const Node& node, std::size_t depth) {
        bool first = true;
        for (const auto index : node.children) {
            if (!first)
                append(options_.layout == RenderLayout::Spaced ? ", " : ",");

            first = false;
            whitespace(depth + 1);
            write(nodes, nodes[static_cast<std::size_t>(index)], depth + 1);
        }

        if (!first)
            whitespace(depth);
    }

    template<class Nodes, class Node>
    void object(const Nodes& nodes, const Node& node, std::size_t depth) {
        bool first = true;
        for (const auto& [name, index] : node.members) {
            if (!first)
                append(options_.layout == RenderLayout::Spaced ? ", " : ",");

            first = false;
            whitespace(depth + 1);
            quoted(name);
            append(options_.layout == RenderLayout::Compact ? ":" : ": ");
            write(nodes, nodes[static_cast<std::size_t>(index)], depth + 1);
        }

        if (!first)
            whitespace(depth);
    }
};

} // namespace detail
} // namespace pipelang::json
