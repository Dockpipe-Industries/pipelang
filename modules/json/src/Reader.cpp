// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "pipelang/json/Reader.hpp"
#include "ValueRenderer.hpp"
#include "UtfInput.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory_resource>
#include <string_view>
#include <utility>

namespace pipelang::json {
namespace {
constexpr std::int64_t maximumInputBytes = 4 * 1024 * 1024;
constexpr std::int64_t maximumNodes = 2097152;
constexpr std::int64_t maximumDepth = 64;
constexpr std::int64_t maximumRetainedBytes = 512 * 1024 * 1024;
std::atomic<std::int64_t> nextDocument{1};
std::atomic<std::int64_t> nextStringIndex{1};
std::atomic<std::size_t> aggregateRetainedBytes{0};

[[noreturn]] void refuse(const char* code) {
    throw ReadFailure(code);
}

void checkLimit(std::int64_t value, std::int64_t maximum) {
    if (value <= 0 || value > maximum)
        refuse("json.limits");
}

struct TextLess {
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const {
        return left < right;
    }
};

struct Node {
    Kind kind;
    std::pmr::string text;
    std::pmr::string name;
    std::pmr::vector<std::int64_t> children;
    std::pmr::map<std::pmr::string, std::int64_t, TextLess> members;
    bool boolean = false;
    bool named = false;

    Node(Kind inputKind, std::pmr::memory_resource* memory)
        : kind(inputKind), text(memory), name(memory), children(memory), members(memory) {}
};

// This ledger is shared by all Reader instances in the linked SDK runtime.
// Credit is returned only after the corresponding storage has been released.
class Reservation {
public:
    explicit Reservation(std::size_t bytes) : bytes_(bytes) {
        auto retained = aggregateRetainedBytes.load();
        do {
            if (bytes > Reader::maximumAggregateRetainedBytes - retained)
                refuse("json.retained_limit");
        } while (!aggregateRetainedBytes.compare_exchange_weak(retained, retained + bytes));
    }

    ~Reservation() { release(bytes_); }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    void release(std::size_t bytes) noexcept {
        aggregateRetainedBytes.fetch_sub(bytes);
        bytes_ -= bytes;
    }

private:
    std::size_t bytes_;
};

// Each index owns a separately reclaimable, fixed arena. Entries borrow the
// immutable decoded document text; there is no second text allocation or copy.
class StringIndex {
    struct Entry {
        std::string_view text;
        Entry* left = nullptr;
        Entry* right = nullptr;
        int height = 1;
    };

public:
    const std::int64_t generation;

    StringIndex(std::int64_t id, StringIndexLimits limits)
        : generation(id), capacity_(static_cast<std::size_t>(limits.entries)),
          reservation_(static_cast<std::size_t>(limits.retainedBytes)),
          storage_(std::make_unique<std::byte[]>(static_cast<std::size_t>(limits.retainedBytes))),
          memory_(storage_.get(), static_cast<std::size_t>(limits.retainedBytes),
                  std::pmr::null_memory_resource()) {}

    bool contains(std::string_view text, const std::function<bool()>& cancelled) const {
        checkpoint(cancelled);
        const Entry* position = root_;
        std::size_t depth = 0;
        while (position) {
            if (depth == maximumPath)
                refuse("json.string_index_state");

            ++depth;
            const int order = compare(text, position->text, cancelled);
            if (order == 0)
                return true;

            position = order < 0 ? position->left : position->right;
        }

        checkpoint(cancelled);
        return false;
    }

    bool add(std::string_view text, const std::function<bool()>& cancelled) {
        checkpoint(cancelled);

        std::array<Entry**, maximumPath> path{};
        std::size_t depth = 0;
        Entry** position = &root_;
        while (*position) {
            const int order = compare(text, (*position)->text, cancelled);
            if (order == 0)
                return false;

            if (depth == path.size())
                refuse("json.string_index_state");

            path[depth++] = position;
            position = order < 0 ? &(*position)->left : &(*position)->right;
        }

        // Equality is checked before capacity so duplicates remain observable
        // even in a full index. Neither cancellation nor allocation failure has
        // changed a link, height, count or prior entry at this point.
        checkpoint(cancelled);
        if (size_ == capacity_)
            refuse("json.string_index_entry_limit");

        void* allocation = memory_.allocate(sizeof(Entry), alignof(Entry));
        auto* entry = std::construct_at(static_cast<Entry*>(allocation), Entry{text});
        *position = entry;
        ++size_;

        // Insertion commits above. Rebalancing cannot fail, allocate, invoke a
        // caller callback or report cancellation after the committed effect.
        while (depth != 0) {
            Entry** ancestor = path[--depth];
            *ancestor = balance(*ancestor);
        }

        return true;
    }

private:
    // An AVL's minimum node count grows at least twofold every two levels.
    static constexpr auto maximumPath = 2 * std::bit_width(static_cast<std::uint64_t>(maximumNodes)) + 1;

    std::size_t capacity_;
    Reservation reservation_;
    std::unique_ptr<std::byte[]> storage_;
    std::pmr::monotonic_buffer_resource memory_;
    Entry* root_ = nullptr;
    std::size_t size_ = 0;

    static void checkpoint(const std::function<bool()>& cancelled) {
        if (cancelled && cancelled())
            refuse("json.cancelled");
    }

    static int compare(std::string_view left, std::string_view right,
                       const std::function<bool()>& cancelled) {
        constexpr std::size_t chunkBytes = 4096;
        const auto common = std::min(left.size(), right.size());
        for (std::size_t offset = 0; offset < common; offset += chunkBytes) {
            checkpoint(cancelled);
            const auto count = std::min(chunkBytes, common - offset);
            const int order = std::memcmp(left.data() + offset, right.data() + offset, count);
            if (order != 0)
                return order;
        }

        checkpoint(cancelled);
        if (left.size() < right.size())
            return -1;

        if (left.size() > right.size())
            return 1;

        return 0;
    }

    static int height(const Entry* entry) noexcept {
        return entry ? entry->height : 0;
    }

    static void update(Entry* entry) noexcept {
        entry->height = 1 + std::max(height(entry->left), height(entry->right));
    }

    static Entry* rotateLeft(Entry* entry) noexcept {
        Entry* replacement = entry->right;
        entry->right = replacement->left;
        replacement->left = entry;
        update(entry);
        update(replacement);
        return replacement;
    }

    static Entry* rotateRight(Entry* entry) noexcept {
        Entry* replacement = entry->left;
        entry->left = replacement->right;
        replacement->right = entry;
        update(entry);
        update(replacement);
        return replacement;
    }

    static Entry* balance(Entry* entry) noexcept {
        update(entry);
        const int difference = height(entry->left) - height(entry->right);
        if (difference > 1) {
            if (height(entry->left->left) < height(entry->left->right))
                entry->left = rotateLeft(entry->left);

            return rotateRight(entry);
        }

        if (difference < -1) {
            if (height(entry->right->right) < height(entry->right->left))
                entry->right = rotateRight(entry->right);

            return rotateLeft(entry);
        }

        return entry;
    }
};

struct Tree {
    std::unique_ptr<std::byte[]> storage;
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::deque<Node> nodes;

    explicit Tree(std::size_t retainedBytes)
        : storage(std::make_unique<std::byte[]>(retainedBytes)),
          memory(storage.get(), retainedBytes, std::pmr::null_memory_resource()), nodes(&memory) {}
};

struct Document {
    std::int64_t generation;
    ReadLimits limits;
    Reservation reservation;
    std::vector<std::uint8_t> input;
    std::unique_ptr<Tree> tree;
    // Destruction order releases the index before the borrowed tree text.
    std::unique_ptr<StringIndex> stringIndex;
    std::int64_t closedStringIndex = 0;
    bool complete = false;
    bool failed = false;
    std::size_t decoded = 0;

    Document(std::int64_t id, ReadLimits bounds)
        : generation(id), limits(bounds),
          reservation(static_cast<std::size_t>(bounds.retainedBytes + bounds.inputBytes)),
          tree(std::make_unique<Tree>(static_cast<std::size_t>(bounds.retainedBytes))) {
        input.reserve(static_cast<std::size_t>(limits.inputBytes));
        if (input.capacity() > static_cast<std::size_t>(limits.inputBytes))
            refuse("json.memory_limit");
    }

    void poison() noexcept {
        failed = true;
        if (tree) {
            tree.reset();
            reservation.release(static_cast<std::size_t>(limits.retainedBytes));
        }
    }

    void charge(std::size_t bytes) {
        if (bytes > static_cast<std::size_t>(limits.decodedBytes) - decoded)
            refuse("json.decoded_limit");
        decoded += bytes;
    }
};

class Builder final : public nlohmann::json_sax<nlohmann::json> {
public:
    Builder(Document& document, const std::function<bool()>& cancelled)
        : document_(document), cancelled_(cancelled) {}

    bool null() override {
        add(Kind::Null);
        return true;
    }
    bool boolean(bool value) override {
        add(Kind::Boolean).boolean = value;
        return true;
    }
    bool number_integer(number_integer_t value) override {
        number(std::to_string(value));
        return true;
    }
    bool number_unsigned(number_unsigned_t value) override {
        number(std::to_string(value));
        return true;
    }
    bool number_float(number_float_t value, const string_t& text) override {
        if (!std::isfinite(value))
            refuse("json.number_range");
        number(text);
        return true;
    }
    bool string(string_t& value) override {
        document_.charge(value.size());
        add(Kind::String).text.assign(value);
        return true;
    }
    bool binary(binary_t&) override {
        refuse("json.syntax");
    }
    bool start_object(std::size_t) override {
        return start(Kind::Object);
    }
    bool start_array(std::size_t) override {
        return start(Kind::Array);
    }
    bool end_object() override {
        return end();
    }
    bool end_array() override {
        return end();
    }
    bool key(string_t& value) override {
        checkpoint();
        if (value.size() > static_cast<std::size_t>(document_.limits.nameBytes))
            refuse("json.name_limit");
        document_.charge(value.size());
        auto& object = document_.tree->nodes[stack_[depth_ - 1]];
        if (object.members.find(value) != object.members.end())
            refuse("json.duplicate_key");
        // Insert before the value: escaped-equivalent names compare decoded UTF8.
        object.members.emplace(std::pmr::string(value, &document_.tree->memory), -1);
        pendingName_.assign(value);
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        refuse("json.syntax");
    }

private:
    Document& document_;
    const std::function<bool()>& cancelled_;
    std::array<std::size_t, maximumDepth> stack_{};
    std::size_t depth_ = 0;
    std::string pendingName_;

    void checkpoint() const {
        if (cancelled_ && cancelled_())
            refuse("json.cancelled");
    }

    Node& add(Kind kind) {
        checkpoint();
        if (document_.tree->nodes.size() >= static_cast<std::size_t>(document_.limits.nodes))
            refuse("json.node_limit");
        const auto index = static_cast<std::int64_t>(document_.tree->nodes.size());
        document_.tree->nodes.emplace_back(kind, &document_.tree->memory);
        auto& node = document_.tree->nodes.back();
        if (depth_ > 0) {
            auto& parent = document_.tree->nodes[stack_[depth_ - 1]];
            parent.children.push_back(index);
            if (parent.kind == Kind::Object) {
                node.named = true;
                node.name.assign(pendingName_);
                parent.members.find(pendingName_)->second = index;
                pendingName_.clear();
            }
        }
        return node;
    }

    void number(const std::string& text) {
        document_.charge(text.size());
        add(Kind::Number).text.assign(text);
    }

    bool start(Kind kind) {
        if (depth_ >= static_cast<std::size_t>(document_.limits.depth))
            refuse("json.depth_limit");
        add(kind);
        stack_[depth_++] = document_.tree->nodes.size() - 1;
        return true;
    }

    bool end() {
        checkpoint();
        --depth_;
        return true;
    }
};

std::vector<std::uint8_t> slice(std::string_view bytes, std::int64_t start, std::int64_t count) {
    if (start < 0 || count < 0 || static_cast<std::uint64_t>(start) > bytes.size() ||
        static_cast<std::uint64_t>(count) > bytes.size() - static_cast<std::size_t>(start) ||
        count > static_cast<std::int64_t>(Reader::maximumFragmentBytes))
        refuse("json.slice");
    const auto fragment = bytes.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(count));
    return {fragment.begin(), fragment.end()};
}
} // namespace

struct Reader::State {
    std::size_t capacity;
    std::array<std::unique_ptr<Document>, maximumDocuments> documents;
    std::int64_t renderingDocument = 0;

    explicit State(std::size_t documents) : capacity(documents) {}
    // One tombstone per slot makes immediately repeated close idempotent without
    // retaining an unbounded history of closed document identities.
    std::array<std::int64_t, maximumDocuments> closed{};

    Document& get(std::int64_t id) const {
        for (const auto& document : documents) {
            if (document && document->generation == id)
                return *document;
        }
        refuse("json.document");
    }

    const Node& node(NodeHandle handle) const {
        const auto& document = get(handle.document);
        if (!document.complete || document.failed)
            refuse("json.not_complete");
        if (handle.node < document.generation * maximumNodes ||
            static_cast<std::uint64_t>(handle.node - document.generation * maximumNodes) >= document.tree->nodes.size())
            refuse("json.node");
        return document.tree->nodes[static_cast<std::size_t>(handle.node - document.generation * maximumNodes)];
    }
};

Reader::Reader(ReaderOptions options) {
    if (options.documents == 0 || options.documents > maximumDocuments)
        refuse("json.limits");
    state_ = std::make_unique<State>(options.documents);
}
Reader::~Reader() = default;

std::int64_t Reader::begin(ReadLimits limits) {
    checkLimit(limits.inputBytes, maximumInputBytes);
    checkLimit(limits.nodes, maximumNodes);
    checkLimit(limits.depth, maximumDepth);
    checkLimit(limits.decodedBytes, maximumInputBytes);
    checkLimit(limits.nameBytes, maximumFragmentBytes);
    checkLimit(limits.retainedBytes, maximumRetainedBytes);
    for (std::size_t index = 0; index < state_->capacity; ++index) {
        if (state_->documents[index])
            continue;
        // Saturate before signed wrap; IDs are never reused in this process.
        auto generation = nextDocument.load();
        do {
            if (generation >= std::numeric_limits<std::int64_t>::max() / maximumNodes)
                refuse("json.generation_limit");
        } while (!nextDocument.compare_exchange_weak(generation, generation + 1));
        try {
            state_->documents[index] = std::make_unique<Document>(generation, limits);
        } catch (const std::bad_alloc&) {
            refuse("json.memory_limit");
        }
        state_->closed[index] = 0;
        return generation;
    }
    refuse("json.document_limit");
}

void Reader::append(std::int64_t id, std::span<const std::uint8_t> bytes) {
    auto& document = state_->get(id);
    if (document.complete || document.failed)
        refuse("json.read_state");
    if (bytes.size() > maximumFragmentBytes ||
        bytes.size() > static_cast<std::size_t>(document.limits.inputBytes) - document.input.size()) {
        document.poison();
        refuse("json.input_limit");
    }
    document.input.insert(document.input.end(), bytes.begin(), bytes.end());
}

NodeHandle Reader::complete(std::int64_t id, const std::function<bool()>& cancelled) {
    auto& document = state_->get(id);
    if (document.complete || document.failed)
        refuse("json.read_state");
    try {
        if (cancelled && cancelled())
            refuse("json.cancelled");
        Builder builder(document, cancelled);
        const auto parse = [&](std::span<const std::uint8_t> bytes) {
            // Disallow a second BOM after stripping one wire prefix.
            if (detail::hasUtf8Bom(bytes))
                refuse("json.encoding");

            // The lexer treats raw NUL as end of input. Refuse it before
            // parsing so no following bytes can hide behind a false marker.
            if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end() ||
                !nlohmann::json::sax_parse(bytes.begin(), bytes.end(), &builder))
                refuse("json.syntax");
        };

        auto input = std::span<const std::uint8_t>(document.input);
        const auto encoding = document.limits.detectUnicodeEncoding
            ? detail::detectInputEncoding(input) : detail::InputEncoding{};
        input = input.subspan(encoding.prefixBytes);
        if (encoding.unitBytes == 1) {
            parse(input);
        } else {
            // UTF-16 produces at most 3 UTF-8 bytes per code unit; UTF-32
            // produces at most 4. Charge the transient decode arena to the
            // existing aggregate reader ledger and release it after parsing.
            const auto capacity = encoding.unitBytes == 2 ? (input.size() / 2) * 3 : input.size();
            Reservation decoded(capacity);
            auto storage = std::make_unique<std::uint8_t[]>(capacity);
            const auto bytes = std::span(storage.get(), capacity);
            const auto size = detail::decodeWideInput(input, encoding, bytes, cancelled);
            parse(bytes.first(size));
        }
        if (cancelled && cancelled())
            refuse("json.cancelled");
        document.complete = true;
        return {id, id * maximumNodes};
    } catch (const std::bad_alloc&) {
        document.poison();
        refuse("json.memory_limit");
    } catch (...) {
        document.poison();
        throw;
    }
}

void Reader::close(std::int64_t id) {
    if (state_->renderingDocument != 0 && id == state_->renderingDocument)
        refuse("json.render.in_use");

    for (std::size_t index = 0; index < maximumDocuments; ++index) {
        if (state_->documents[index] && state_->documents[index]->generation == id) {
            state_->documents[index].reset();
            state_->closed[index] = id;
            return;
        }
        if (id > 0 && state_->closed[index] == id)
            return;
    }
    refuse("json.document");
}

std::size_t Reader::aggregateRetainedBytes() noexcept {
    return pipelang::json::aggregateRetainedBytes.load();
}

std::size_t Reader::liveDocuments() const noexcept {
    std::size_t result = 0;
    for (const auto& document : state_->documents) {
        if (document)
            ++result;
    }
    return result;
}

StringIndexHandle Reader::beginStringIndex(std::int64_t id, StringIndexLimits limits) {
    auto& document = state_->get(id);
    if (!document.complete || document.failed)
        refuse("json.not_complete");

    if (document.stringIndex)
        refuse("json.string_index_limit");

    checkLimit(limits.entries, document.limits.nodes);
    checkLimit(limits.retainedBytes, static_cast<std::int64_t>(maximumAggregateRetainedBytes));
    auto generation = nextStringIndex.load();
    do {
        if (generation == std::numeric_limits<std::int64_t>::max())
            refuse("json.generation_limit");
    } while (!nextStringIndex.compare_exchange_weak(generation, generation + 1));

    try {
        document.stringIndex = std::make_unique<StringIndex>(generation, limits);
    } catch (const std::bad_alloc&) {
        refuse("json.memory_limit");
    }

    document.closedStringIndex = 0;
    return {id, generation};
}

bool Reader::addString(StringIndexHandle handle, NodeHandle node,
                       const std::function<bool()>& cancelled) {
    auto& document = state_->get(handle.document);
    if (!document.stringIndex || document.stringIndex->generation != handle.generation)
        refuse("json.string_index");

    if (node.document != handle.document)
        refuse("json.node");

    const auto& value = state_->node(node);
    if (value.kind != Kind::String)
        refuse("json.kind");

    try {
        return document.stringIndex->add(value.text, cancelled);
    } catch (const std::bad_alloc&) {
        refuse("json.memory_limit");
    }
}

bool Reader::containsString(StringIndexHandle handle, NodeHandle node,
                            const std::function<bool()>& cancelled) const {
    const auto& document = state_->get(handle.document);
    if (!document.stringIndex || document.stringIndex->generation != handle.generation)
        refuse("json.string_index");

    const auto& value = state_->node(node);
    if (value.kind != Kind::String)
        refuse("json.kind");

    return document.stringIndex->contains(value.text, cancelled);
}

void Reader::closeStringIndex(StringIndexHandle handle) {
    auto& document = state_->get(handle.document);
    if (document.stringIndex && document.stringIndex->generation == handle.generation) {
        document.stringIndex.reset();
        document.closedStringIndex = handle.generation;
        return;
    }

    if (handle.generation > 0 && document.closedStringIndex == handle.generation)
        return;

    refuse("json.string_index");
}

std::size_t Reader::render(NodeHandle source, RenderOptions options, const JsonByteSink& sink,
                           const std::function<bool()>& cancelled) const {
    if (!sink || options.maximumBytes == 0 || options.maximumBytes > maximumRenderBytes ||
        options.initialDepth > maximumRenderInitialDepth ||
        (options.layout != RenderLayout::Compact && options.layout != RenderLayout::Spaced &&
         options.layout != RenderLayout::Indented))
        refuse("json.render.options");
    if (state_->renderingDocument != 0)
        refuse("json.render.in_use");

    const auto& node = state_->node(source);
    const auto& nodes = state_->get(source.document).tree->nodes;
    state_->renderingDocument = source.document;
    struct RenderGuard {
        std::int64_t& document;
        ~RenderGuard() { document = 0; }
    } guard{state_->renderingDocument};

    detail::ValueRenderer renderer(options, sink, cancelled);
    renderer.write(nodes, node, options.initialDepth);
    return renderer.finish();
}

Kind Reader::kind(NodeHandle node) const {
    return state_->node(node).kind;
}

std::int64_t Reader::count(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Object && value.kind != Kind::Array)
        refuse("json.kind");
    return static_cast<std::int64_t>(value.children.size());
}

NodeHandle Reader::at(NodeHandle node, std::int64_t index) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Object && value.kind != Kind::Array)
        refuse("json.kind");
    if (index < 0 || static_cast<std::uint64_t>(index) >= value.children.size())
        refuse("json.index");
    return {node.document, node.document * maximumNodes + value.children[static_cast<std::size_t>(index)]};
}

std::optional<NodeHandle> Reader::find(NodeHandle node, const std::string& name) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Object)
        refuse("json.kind");
    const auto member = value.members.find(name);
    if (member == value.members.end())
        return std::nullopt;
    return NodeHandle{node.document, node.document * maximumNodes + member->second};
}

bool Reader::boolean(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Boolean)
        refuse("json.kind");
    return value.boolean;
}

std::int64_t Reader::integer(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Number)
        refuse("json.kind");
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.text.data() + value.text.size())
        refuse("json.integer_range");
    return result;
}

double Reader::finiteFloat(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::Number)
        refuse("json.kind");
    double result = 0;
    const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.text.data() + value.text.size() || !std::isfinite(result))
        refuse("json.number_range");
    return result;
}

std::int64_t Reader::scalarLength(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (value.kind != Kind::String && value.kind != Kind::Number)
        refuse("json.kind");
    return static_cast<std::int64_t>(value.text.size());
}

std::vector<std::uint8_t> Reader::scalarSlice(NodeHandle node, std::int64_t start, std::int64_t count) const {
    scalarLength(node);
    return slice(state_->node(node).text, start, count);
}

std::int64_t Reader::nameLength(NodeHandle node) const {
    const auto& value = state_->node(node);
    if (!value.named)
        refuse("json.no_name");
    return static_cast<std::int64_t>(value.name.size());
}

std::vector<std::uint8_t> Reader::nameSlice(NodeHandle node, std::int64_t start, std::int64_t count) const {
    nameLength(node);
    return slice(state_->node(node).name, start, count);
}

std::int64_t Reader::inputLength(std::int64_t id) const {
    return static_cast<std::int64_t>(state_->get(id).input.size());
}

std::vector<std::uint8_t> Reader::inputSlice(std::int64_t id, std::int64_t start, std::int64_t count) const {
    const auto& input = state_->get(id).input;
    return slice(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()), start, count);
}

std::size_t Reader::retainedCapacity(std::int64_t id) const {
    const auto& document = state_->get(id);
    return (document.tree ? static_cast<std::size_t>(document.limits.retainedBytes) : 0) +
           document.input.capacity();
}
} // namespace pipelang::json
