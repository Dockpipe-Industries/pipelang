// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pipelang::json {

class ReadFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ReadLimits {
    std::int64_t inputBytes = 4 * 1024 * 1024;
    std::int64_t nodes = 131072;
    std::int64_t depth = 64;
    std::int64_t decodedBytes = 4 * 1024 * 1024;
    std::int64_t nameBytes = 1024 * 1024;
    std::int64_t retainedBytes = 32 * 1024 * 1024;
    bool detectUnicodeEncoding = false;
};

struct ReaderOptions {
    std::size_t documents = 8;
};

struct NodeHandle {
    std::int64_t document = 0;
    std::int64_t node = 0;
};

struct StringIndexHandle {
    std::int64_t document = 0;
    std::int64_t generation = 0;
};

// Both limits are explicit. No index arena is reserved until beginStringIndex.
struct StringIndexLimits {
    std::int64_t entries = 0;
    std::int64_t retainedBytes = 0;
};

enum class Kind { Null, Boolean, Number, String, Object, Array };

enum class RenderLayout { Compact, Spaced, Indented };

struct RenderOptions {
    std::size_t maximumBytes;
    RenderLayout layout = RenderLayout::Compact;
    bool ascii = false;
    std::size_t initialDepth = 0;
};

using JsonByteSink = std::function<void(std::span<const std::uint8_t>)>;

// Single-owner, synchronous service. A caller serializes access. Every document
// has a process-unique generation; handles from another Reader are refused.
class Reader {
public:
    static constexpr std::size_t maximumDocuments = 16;
    static constexpr std::size_t maximumAggregateRetainedBytes = 640 * 1024 * 1024;
    static constexpr std::size_t maximumFragmentBytes = 1024 * 1024;
    static constexpr std::size_t maximumRenderBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t maximumRenderInitialDepth = 64;

    explicit Reader(ReaderOptions options = {});
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    std::int64_t begin(ReadLimits limits);
    void append(std::int64_t document, std::span<const std::uint8_t> bytes);
    NodeHandle complete(std::int64_t document, const std::function<bool()>& cancelled = {});
    void close(std::int64_t document);
    std::size_t liveDocuments() const noexcept;
    static std::size_t aggregateRetainedBytes() noexcept;

    StringIndexHandle beginStringIndex(std::int64_t document, StringIndexLimits limits);
    bool addString(StringIndexHandle index, NodeHandle node,
                   const std::function<bool()>& cancelled = {});
    bool containsString(StringIndexHandle index, NodeHandle node,
                        const std::function<bool()>& cancelled = {}) const;
    void closeStringIndex(StringIndexHandle index);

    // Serialize a borrowed value in decoded-key order. Numeric values use
    // the reader's stored numeric spelling. Sink fragments may split UTF-8
    // sequences. A failure may leave a prefix in the sink.
    std::size_t render(NodeHandle source, RenderOptions options, const JsonByteSink& sink,
                       const std::function<bool()>& cancelled = {}) const;

    Kind kind(NodeHandle node) const;
    std::int64_t count(NodeHandle node) const;
    NodeHandle at(NodeHandle node, std::int64_t index) const;
    std::optional<NodeHandle> find(NodeHandle node, const std::string& name) const;
    bool boolean(NodeHandle node) const;
    std::int64_t integer(NodeHandle node) const;
    double finiteFloat(NodeHandle node) const;
    std::int64_t scalarLength(NodeHandle node) const;
    std::vector<std::uint8_t> scalarSlice(NodeHandle node, std::int64_t start, std::int64_t count) const;
    std::int64_t nameLength(NodeHandle node) const;
    std::vector<std::uint8_t> nameSlice(NodeHandle node, std::int64_t start, std::int64_t count) const;
    std::int64_t inputLength(std::int64_t document) const;
    std::vector<std::uint8_t> inputSlice(std::int64_t document, std::int64_t start, std::int64_t count) const;
    std::size_t retainedCapacity(std::int64_t document) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace pipelang::json
