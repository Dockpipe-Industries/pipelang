// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "pipelang/json/DocumentWriter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>

namespace pipelang::json {

DocumentWriter::DocumentWriter(Reader& reader, RenderOptions options, JsonByteSink sink)
    : DocumentWriter(reader, options, std::move(sink), ValueCapture{}) {
    if (options_.initialDepth != 0)
        throw ReadFailure("json.writer.options");
}

DocumentWriter::DocumentWriter(Reader& reader, RenderOptions options, JsonByteSink sink, ValueCapture)
    : reader_(reader), options_(options), sink_(std::move(sink)) {
    if (!sink_ || options_.maximumBytes == 0 || options_.maximumBytes > Reader::maximumRenderBytes ||
        options_.initialDepth > frames_.size() ||
        (options_.layout != RenderLayout::Compact && options_.layout != RenderLayout::Spaced &&
         options_.layout != RenderLayout::Indented))
        throw ReadFailure("json.writer.options");
}

DocumentWriter::CapturedValue DocumentWriter::capture(Reader& reader, RenderOptions options,
    const ValueBuilder& build, const std::function<bool()>& cancelled) {
    if (!build)
        throw ReadFailure("json.writer.options");
    if (cancelled && cancelled())
        throw ReadFailure("json.cancelled");

    CapturedValue value(options);
    auto& buffer = value.storage_->bytes;
    DocumentWriter writer(reader, options, [&](std::span<const std::uint8_t> bytes) {
        if (cancelled && cancelled())
            throw ReadFailure("json.cancelled");

        const auto required = buffer.size() + bytes.size();
        if (required > buffer.capacity()) {
            const auto growth = std::max(bytes.size(), buffer.capacity());
            buffer.reserve(std::min(options.maximumBytes, buffer.size() + growth));
        }

        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    }, ValueCapture{});
    build(writer);
    if (cancelled && cancelled())
        throw ReadFailure("json.cancelled");

    (void)writer.complete(false);
    return value;
}

void DocumentWriter::emit(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > options_.maximumBytes - delivered_)
        throw ReadFailure("json.writer.byte_limit");

    constexpr std::size_t fragmentLimit = 4096;
    while (!bytes.empty()) {
        const auto size = std::min(bytes.size(), fragmentLimit);
        delivered_ += size;
        sink_(bytes.first(size));
        bytes = bytes.subspan(size);
    }
}

void DocumentWriter::emit(std::string_view text) {
    emit(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void DocumentWriter::padding(std::size_t depth) {
    if (options_.layout != RenderLayout::Indented)
        return;

    emit("\n");
    constexpr std::string_view spaces = "                                ";
    for (auto remaining = (depth + options_.initialDepth) * 2; remaining != 0;) {
        const auto count = std::min(remaining, spaces.size());
        emit(spaces.substr(0, count));
        remaining -= count;
    }
}

void DocumentWriter::separator(Frame& frame) {
    if (frame.populated)
        emit(options_.layout == RenderLayout::Spaced ? ", " : ",");

    frame.populated = true;
    padding(depth_);
}

void DocumentWriter::beforeValue() {
    if (depth_ == 0) {
        if (started_)
            throw ReadFailure("json.writer.root");

        started_ = true;
        return;
    }

    auto& parent = frames_[depth_ - 1];
    if (parent.kind == Container::Array) {
        separator(parent);
        return;
    }
    if (!parent.namePending)
        throw ReadFailure("json.writer.name");

    parent.namePending = false;
}

void DocumentWriter::start(Container kind) {
    if (depth_ == frames_.size() - options_.initialDepth)
        throw ReadFailure("json.writer.depth");

    beforeValue();
    emit(kind == Container::Object ? "{" : "[");
    frames_[depth_++] = Frame{kind};
}

void DocumentWriter::close(Container kind) {
    if (depth_ == 0)
        throw ReadFailure("json.writer.container");

    const auto& frame = frames_[depth_ - 1];
    if (frame.kind != kind || frame.namePending)
        throw ReadFailure("json.writer.container");

    --depth_;
    if (frame.populated)
        padding(depth_);

    emit(kind == Container::Object ? "}" : "]");
}

void DocumentWriter::quoted(std::string_view value) {
    if (value.size() > Reader::maximumFragmentBytes)
        throw ReadFailure("json.writer.text_limit");

    try {
        // One scalar is bounded independently of the document and validated by
        // the SDK's existing UTF-8 serialization dependency.
        emit(nlohmann::json(std::string(value)).dump(-1, ' ', options_.ascii));
    } catch (const nlohmann::json::type_error&) {
        throw ReadFailure("json.writer.utf8");
    }
}

void DocumentWriter::beginObject() {
    write([&] { start(Container::Object); });
}

void DocumentWriter::beginArray() {
    write([&] { start(Container::Array); });
}

void DocumentWriter::name(std::string_view text) {
    write([&] {
        if (depth_ == 0)
            throw ReadFailure("json.writer.name");

        auto& frame = frames_[depth_ - 1];
        if (frame.kind != Container::Object || frame.namePending)
            throw ReadFailure("json.writer.name");

        separator(frame);
        quoted(text);
        emit(options_.layout == RenderLayout::Compact ? ":" : ": ");
        frame.namePending = true;
    });
}

void DocumentWriter::endObject() {
    write([&] { close(Container::Object); });
}

void DocumentWriter::endArray() {
    write([&] { close(Container::Array); });
}

void DocumentWriter::text(std::string_view value) {
    write([&] {
        beforeValue();
        quoted(value);
    });
}

void DocumentWriter::integer(std::int64_t value) {
    write([&] {
        beforeValue();
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec != std::errc{})
            throw ReadFailure("json.writer.number");

        emit(std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    });
}

void DocumentWriter::finite(double value) {
    write([&] {
        if (!std::isfinite(value))
            throw ReadFailure("json.writer.number");

        beforeValue();
        emit(nlohmann::json(value).dump());
    });
}

void DocumentWriter::boolean(bool value) {
    write([&] {
        beforeValue();
        emit(value ? "true" : "false");
    });
}

void DocumentWriter::nullValue() {
    write([&] {
        beforeValue();
        emit("null");
    });
}

void DocumentWriter::borrowed(NodeHandle value, const std::function<bool()>& cancelled) {
    write([&] {
        if (cancelled && cancelled())
            throw ReadFailure("json.cancelled");

        beforeValue();
        RenderOptions selected{options_.maximumBytes - delivered_, options_.layout, options_.ascii, depth_ + options_.initialDepth};
        if (selected.maximumBytes == 0)
            throw ReadFailure("json.writer.byte_limit");

        reader_.render(value, selected, [&](std::span<const std::uint8_t> chunk) { emit(chunk); }, cancelled);
    });
}

void DocumentWriter::captured(const CapturedValue& value, const std::function<bool()>& cancelled) {
    write([&] {
        if (cancelled && cancelled())
            throw ReadFailure("json.cancelled");

        if (!value.storage_)
            throw ReadFailure("json.writer.capture_placement");

        const auto& stored = *value.storage_;
        if (stored.layout.layout != options_.layout || stored.layout.ascii != options_.ascii ||
            stored.layout.initialDepth != depth_ + options_.initialDepth)
            throw ReadFailure("json.writer.capture_placement");

        beforeValue();
        if (stored.bytes.size() > options_.maximumBytes - delivered_)
            throw ReadFailure("json.writer.byte_limit");

        std::span<const std::uint8_t> bytes(stored.bytes);
        constexpr std::size_t batchBytes = 4096;
        while (!bytes.empty()) {
            if (cancelled && cancelled())
                throw ReadFailure("json.cancelled");

            const auto count = std::min(bytes.size(), batchBytes);
            emit(bytes.first(count));
            bytes = bytes.subspan(count);
        }

        if (cancelled && cancelled())
            throw ReadFailure("json.cancelled");
    });
}

std::size_t DocumentWriter::finish() {
    return complete(true);
}

std::size_t DocumentWriter::complete(bool newline) {
    return write([&] {
        if (!started_ || depth_ != 0)
            throw ReadFailure("json.writer.incomplete");

        if (newline)
            emit("\n");
        usable_ = false;
        return delivered_;
    });
}

} // namespace pipelang::json
