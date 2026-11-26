// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace pipelang::json {

// One structural document per already selected sink. An operation failure
// invalidates the writer. The caller independently abandons its staging sink.
class DocumentWriter final {
public:
    // A complete value captured for one output layout and parent depth.
    class CapturedValue final {
    public:
        CapturedValue(CapturedValue&&) noexcept = default;
        CapturedValue& operator=(CapturedValue&&) noexcept = default;
        CapturedValue(const CapturedValue&) = delete;
        CapturedValue& operator=(const CapturedValue&) = delete;

        std::size_t size() const noexcept {
            return storage_ ? storage_->bytes.size() : 0;
        }

    private:
        friend class DocumentWriter;

        struct Storage {
            RenderOptions layout;
            std::vector<std::uint8_t> bytes;
        };

        explicit CapturedValue(RenderOptions layout)
            : storage_(std::make_unique<Storage>(Storage{layout, {}})) {}

        std::unique_ptr<Storage> storage_;
    };

    using ValueBuilder = std::function<void(DocumentWriter&)>;

    static CapturedValue capture(Reader& reader, RenderOptions options, const ValueBuilder& build,
                                 const std::function<bool()>& cancelled = {});

    DocumentWriter(Reader& reader, RenderOptions options, JsonByteSink sink);
    DocumentWriter(const DocumentWriter&) = delete;
    DocumentWriter& operator=(const DocumentWriter&) = delete;

    void beginObject();
    void beginArray();
    void name(std::string_view text);
    void endObject();
    void endArray();
    void text(std::string_view value);
    void integer(std::int64_t value);
    void finite(double value);
    void boolean(bool value);
    void nullValue();
    void borrowed(NodeHandle value, const std::function<bool()>& cancelled = {});
    void captured(const CapturedValue& value, const std::function<bool()>& cancelled = {});
    std::size_t finish();

private:
    struct ValueCapture {};
    DocumentWriter(Reader& reader, RenderOptions options, JsonByteSink sink, ValueCapture);

    enum class Container { Object, Array };

    struct Frame {
        Container kind = Container::Object;
        bool populated = false;
        bool namePending = false;
    };

    Reader& reader_;
    RenderOptions options_;
    JsonByteSink sink_;
    std::array<Frame, 64> frames_{};
    std::size_t depth_ = 0;
    std::size_t delivered_ = 0;
    bool started_ = false;
    bool usable_ = true;
    bool busy_ = false;

    template<class Operation>
    decltype(auto) write(Operation&& operation) {
        if (busy_ || !usable_)
            throw ReadFailure("json.writer.state");

        busy_ = true;
        struct WriteGuard {
            bool& busy;
            ~WriteGuard() { busy = false; }
        } guard{busy_};

        try {
            return std::forward<Operation>(operation)();
        } catch (...) {
            usable_ = false;
            throw;
        }
    }

    void emit(std::span<const std::uint8_t> bytes);
    void emit(std::string_view text);
    void padding(std::size_t depth);
    void separator(Frame& frame);
    void beforeValue();
    void start(Container kind);
    void close(Container kind);
    void quoted(std::string_view value);
    std::size_t complete(bool newline);
};

} // namespace pipelang::json
