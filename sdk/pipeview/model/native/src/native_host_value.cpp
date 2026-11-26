// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "native_host_projection.hpp"

#include "NativeLists.hpp"
#include "NativeReceiverStorage.hpp"
#include "NativeReceivers.hpp"

#include <set>
#include <bit>
#include <limits>

namespace pipelang::native_app_v19 {

bool ReceiverReferenceDomain::owns(const void* receiver) const
{
    return receiver && (receiver == root || receivers.owns(receiver));
}

std::shared_ptr<const ExportedReceiverOwner> ExportedReceiverReference::requireOwner() const
{
    const auto lifetime = owner.lock();
    if (!lifetime)
        throw model::ExecutionFailure("object domain is closed");
    if (lifetime->thread != std::this_thread::get_id())
        throw model::ExecutionFailure("object accessed from a different owner thread");
    return lifetime;
}

struct HostedValue::Node {
    Kind kind = Kind::Void;
    bool exportedReference = false;
    std::int64_t number = 0;
    std::string type;
    std::string text;
    std::vector<HostedValue> children;
    std::vector<std::string> names;
};

struct HostedValue::ExportedObjectNode : Node {
    std::shared_ptr<const ExportedReceiverReference> reference;
};

struct HostedValue::SemaphoreNode : Node {
    explicit SemaphoreNode(model::SemaphoreSlim value) : semaphore(std::move(value))
    {
        kind = Kind::Semaphore;
        type = "SemaphoreSlim";
    }

    model::SemaphoreSlim semaphore;
};

struct HostedValue::ConcurrentIntegerQueueNode : Node {
    explicit ConcurrentIntegerQueueNode(model::ConcurrentQueue<std::int64_t> value) : queue(std::move(value))
    {
        kind = Kind::ConcurrentIntegerQueue;
        type = "ConcurrentQueue<int>";
    }

    model::ConcurrentQueue<std::int64_t> queue;
};

struct HostedValue::ConcurrentSequenceNode : Node {
    ConcurrentSequenceNode(std::string name, std::shared_ptr<const detail::HostedSequenceCell> cell)
        : sequence(std::move(cell))
    {
        kind = Kind::ConcurrentSequence;
        type = std::move(name);
    }

    std::shared_ptr<const detail::HostedSequenceCell> sequence;
};

struct HostedValue::ConcurrentDictionaryNode : Node {
    ConcurrentDictionaryNode(std::string name, std::shared_ptr<const detail::HostedDictionaryCell> cell)
        : dictionary(std::move(cell))
    {
        kind = Kind::ConcurrentDictionary;
        type = std::move(name);
    }

    std::shared_ptr<const detail::HostedDictionaryCell> dictionary;
};

struct HostedValue::LockNode : Node {
    explicit LockNode(model::Lock value) : lock(std::move(value))
    {
        kind = Kind::Lock;
        type = "Lock";
    }

    model::Lock lock;
};

struct HostedValue::AtomicNode : Node {
    explicit AtomicNode(HostedAtomic value) : atomic(std::move(value))
    {
        kind = Kind::Atomic;
        type = std::visit([](const auto& cell) -> std::string_view {
            using Cell = std::decay_t<decltype(cell)>;
            cell.requireCell();
            if constexpr (std::is_same_v<Cell, model::Atomic<std::int64_t>>)
                return "Atomic<int>";
            else if constexpr (std::is_same_v<Cell, model::Atomic<bool>>)
                return "Atomic<bool>";
            else if constexpr (std::is_same_v<Cell, model::Atomic<double>>)
                return "Atomic<float>";
            else
                return "Atomic<float32>";
        }, atomic);
    }

    HostedAtomic atomic;
};

HostedValue::HostedValue(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

HostedValue HostedValue::fromSemaphore(model::SemaphoreSlim semaphore)
{
    semaphore.requireCell();
    return HostedValue(std::make_shared<SemaphoreNode>(std::move(semaphore)));
}

model::SemaphoreSlim HostedValue::semaphore() const
{
    return static_cast<const SemaphoreNode&>(require(Kind::Semaphore)).semaphore;
}

HostedValue HostedValue::fromConcurrentQueue(model::ConcurrentQueue<std::int64_t> queue)
{
    queue.requireCell();
    return HostedValue(std::make_shared<ConcurrentIntegerQueueNode>(std::move(queue)));
}

model::ConcurrentQueue<std::int64_t> HostedValue::concurrentQueue() const
{
    return static_cast<const ConcurrentIntegerQueueNode&>(require(Kind::ConcurrentIntegerQueue)).queue;
}

HostedValue HostedValue::fromHostedSequence(std::string type,
    std::shared_ptr<const detail::HostedSequenceCell> sequence)
{
    return HostedValue(std::make_shared<ConcurrentSequenceNode>(std::move(type), std::move(sequence)));
}

std::shared_ptr<const detail::HostedSequenceCell> HostedValue::hostedSequence() const
{
    return static_cast<const ConcurrentSequenceNode&>(require(Kind::ConcurrentSequence)).sequence;
}

HostedValue HostedValue::fromHostedDictionary(std::string type,
    std::shared_ptr<const detail::HostedDictionaryCell> dictionary)
{
    return HostedValue(std::make_shared<ConcurrentDictionaryNode>(std::move(type), std::move(dictionary)));
}

std::shared_ptr<const detail::HostedDictionaryCell> HostedValue::hostedDictionary() const
{
    return static_cast<const ConcurrentDictionaryNode&>(require(Kind::ConcurrentDictionary)).dictionary;
}

HostedValue HostedValue::fromLock(model::Lock lock)
{
    lock.requireCell();
    return HostedValue(std::make_shared<LockNode>(std::move(lock)));
}

model::Lock HostedValue::lock() const
{
    return static_cast<const LockNode&>(require(Kind::Lock)).lock;
}

HostedValue HostedValue::fromAtomic(HostedAtomic atomic)
{
    return HostedValue(std::make_shared<AtomicNode>(std::move(atomic)));
}

HostedAtomic HostedValue::atomic() const
{
    return static_cast<const AtomicNode&>(require(Kind::Atomic)).atomic;
}

bool HostedValue::containsObjectReference() const
{
    if (!node_)
        return false;

    std::vector<const Node*> pending{node_.get()};
    std::set<const Node*> visited;
    while (!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();
        if (!visited.insert(node).second)
            continue;
        if (node->kind == Kind::ObjectReference)
            return true;
        for (const auto& child : node->children) {
            if (child.node_)
                pending.push_back(child.node_.get());
        }
    }
    return false;
}

bool HostedValue::sameObject(const HostedValue& other) const
{
    const auto& left = require(Kind::ObjectReference);
    const auto& right = other.require(Kind::ObjectReference);
    if (!left.exportedReference || !right.exportedReference)
        throw model::ExecutionFailure("cannot compare internal native receiver references outside their binding");
    const auto& first = *static_cast<const ExportedObjectNode&>(left).reference;
    const auto& second = *static_cast<const ExportedObjectNode&>(right).reference;
    const auto firstOwner = first.requireOwner();
    const auto secondOwner = second.requireOwner();
    return firstOwner == secondOwner && first.address == second.address;
}

HostedValue HostedValue::fromInteger(std::int64_t value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Integer;
    node->type = "int";
    node->number = value;
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromUInt32(std::uint32_t value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::UInt32;
    node->type = "uint32";
    node->number = value;
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromFloat(double value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Float;
    node->type = "float";
    node->number = std::bit_cast<std::int64_t>(value);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromFloat32(float value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Float32;
    node->type = "float32";
    node->number = std::bit_cast<std::uint32_t>(value);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromBytes(std::span<const std::uint8_t> value)
{
    model::byte_buffer::admitAppend(0, value.size());
    auto node = std::make_shared<Node>();
    node->kind = Kind::ByteBuffer;
    node->type = "ByteBuffer";
    node->text.assign(NativeBytes::asText(value));
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromBoolean(bool value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Boolean;
    node->type = "bool";
    node->number = value;
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromText(std::string value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Text;
    node->type = "string";
    node->text = std::move(value);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromError(std::string message)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Error;
    node->type = "Error";
    node->text = std::move(message);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromRecord(std::string type, std::vector<std::pair<std::string, HostedValue>> fields)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Record;
    node->type = std::move(type);
    node->names.reserve(fields.size());
    node->children.reserve(fields.size());
    for (auto& [name, value] : fields) {
        node->names.push_back(std::move(name));
        node->children.push_back(std::move(value));
    }
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromEnum(std::string type, std::int64_t ordinal, std::string member)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Enum;
    node->type = std::move(type);
    node->number = ordinal;
    node->text = std::move(member);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromChildren(Kind kind, std::string type, std::vector<HostedValue> children)
{
    auto node = std::make_shared<Node>();
    node->kind = kind;
    node->type = std::move(type);
    node->children = std::move(children);
    return HostedValue(std::move(node));
}

HostedValue HostedValue::fromList(std::string type, std::vector<HostedValue> items)
{
    return fromChildren(Kind::List, std::move(type), std::move(items));
}

HostedValue HostedValue::fromHashSet(std::string type, std::vector<HostedValue> items)
{
    return fromChildren(Kind::HashSet, std::move(type), std::move(items));
}

HostedValue HostedValue::fromQueue(std::string type, std::vector<HostedValue> items)
{
    return fromChildren(Kind::Queue, std::move(type), std::move(items));
}

HostedValue HostedValue::fromStack(std::string type, std::vector<HostedValue> topFirst)
{
    return fromChildren(Kind::Stack, std::move(type), std::move(topFirst));
}

HostedValue HostedValue::fromDictionary(std::string type, std::vector<HostedValue> entries)
{
    return fromChildren(Kind::Dictionary, std::move(type), std::move(entries));
}

HostedValue HostedValue::fromOptional(std::string type, HostedValue value)
{
    std::vector<HostedValue> children;
    if (value.kind() != Kind::Void)
        children.push_back(std::move(value));
    return fromChildren(Kind::Optional, std::move(type), std::move(children));
}

HostedValue HostedValue::fromResult(std::string type, model::Outcome outcome, HostedValue value)
{
    auto node = std::make_shared<Node>();
    node->kind = Kind::Result;
    node->type = std::move(type);
    node->number = static_cast<std::int64_t>(outcome);
    if (outcome == model::Outcome::Success) {
        node->children.emplace_back();
        if (value.kind() != Kind::Void)
            node->children.push_back(std::move(value));
    } else {
        node->children.push_back(std::move(value));
    }
    return HostedValue(std::move(node));
}

const HostedValue::Node& HostedValue::require(Kind expected) const
{
    if (!node_ || node_->kind != expected)
        throw model::ExecutionFailure("native hosted value has a different kind");
    return *node_;
}

HostedValue::Kind HostedValue::kind() const { return node_ ? node_->kind : Kind::Void; }
std::string_view HostedValue::typeName() const { return node_ ? std::string_view(node_->type) : "void"; }
std::int64_t HostedValue::integer() const { return require(Kind::Integer).number; }
std::uint32_t HostedValue::uint32() const { return static_cast<std::uint32_t>(require(Kind::UInt32).number); }
double HostedValue::floating() const { return std::bit_cast<double>(require(Kind::Float).number); }
float HostedValue::float32() const
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(require(Kind::Float32).number));
}
std::span<const std::uint8_t> HostedValue::bytes() const
{
    const auto& value = require(Kind::ByteBuffer).text;
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}
bool HostedValue::boolean() const { return require(Kind::Boolean).number != 0; }

std::string_view HostedValue::text() const
{
    if (kind() == Kind::Error)
        return require(Kind::Error).text;
    return require(Kind::Text).text;
}

std::int64_t HostedValue::ordinal() const { return require(Kind::Enum).number; }
std::string_view HostedValue::enumerator() const { return require(Kind::Enum).text; }

HostedValue HostedValue::field(std::string_view name) const
{
    const auto& record = require(Kind::Record);
    const auto field = std::find(record.names.begin(), record.names.end(), name);
    if (field == record.names.end())
        throw model::ExecutionFailure("native hosted record has no such field");
    return record.children[static_cast<std::size_t>(field - record.names.begin())];
}

std::size_t HostedValue::size() const
{
    return collectionItems().size();
}

HostedValue HostedValue::at(std::size_t index) const
{
    const auto& items = collectionItems();
    if (index >= items.size())
        throw model::ExecutionFailure("native hosted list index outside admitted range");
    return items[index];
}

const std::vector<HostedValue>& HostedValue::collectionItems() const
{
    switch (kind()) {
    case Kind::List:
    case Kind::HashSet:
    case Kind::Queue:
    case Kind::Stack:
    case Kind::Dictionary:
        return node_->children;
    default:
        return require(Kind::List).children;
    }
}

bool HostedValue::hasValue() const { return !require(Kind::Optional).children.empty(); }

const HostedValue::Node& HostedValue::requireResult() const
{
    if (kind() != Kind::ResultInterface)
        return require(Kind::Result);
    const auto& result = require(Kind::ResultInterface);
    if (result.children.empty())
        throw model::ExecutionFailure("result interface is empty");
    return result;
}

model::Outcome HostedValue::outcome() const
{
    return static_cast<model::Outcome>(requireResult().number);
}

HostedValue HostedValue::value() const
{
    if (kind() == Kind::Optional) {
        if (!hasValue())
            throw model::ExecutionFailure("Optional.Value requires Some");
        return node_->children.front();
    }
    const auto& result = require(Kind::Result);
    if (outcome() != model::Outcome::Success)
        throw model::ExecutionFailure("Result.Value requires Success");
    if (result.children.size() == 1)
        return {};
    return result.children[1];
}

HostedValue HostedValue::error() const { return requireResult().children.front(); }

TextView HostedInput::retainText(std::string_view source)
{
    constexpr auto maximumBytes = 32 * model::maximumListBytes;
    if (source.size() > maximumBytes - textBytes_)
        throw model::ExecutionFailure("native hosted input exceeds the text storage limit");
    model::validateText(source);
    if (source.empty())
        return {};
    const auto& owned = text_.emplace_back(source);
    textBytes_ += owned.size();
    return {owned.data(), owned.size()};
}

ByteView HostedInput::retainBytes(std::span<const std::uint8_t> source, const void* reusableSlot)
{
    constexpr auto maximumBytes = 32 * model::maximumListBytes;
    if (reusableSlot) {
        const auto found = reusableBytes_.find(reusableSlot);
        if (found != reusableBytes_.end()) {
            // The compiler proved that the previous value has no surviving aliases.
            const auto otherBytes = textBytes_ - found->second.size();
            if (source.size() > maximumBytes - otherBytes)
                throw model::ExecutionFailure("native hosted input exceeds the byte storage limit");
            found->second = std::string(NativeBytes::asText(source));
            textBytes_ = otherBytes + found->second.size();
            return NativeBytes::borrowed(found->second);
        }
        if (!source.empty() && reusableBytes_.size() < model::maximumListElements) {
            if (source.size() > maximumBytes - textBytes_)
                throw model::ExecutionFailure("native hosted input exceeds the byte storage limit");
            const auto& owned = reusableBytes_.try_emplace(reusableSlot, NativeBytes::asText(source)).first->second;
            textBytes_ += owned.size();
            return NativeBytes::borrowed(owned);
        }
    }
    if (source.size() > maximumBytes - textBytes_)
        throw model::ExecutionFailure("native hosted input exceeds the byte storage limit");
    if (source.empty())
        return {};
    const auto& owned = text_.emplace_back(NativeBytes::asText(source));
    textBytes_ += owned.size();
    return NativeBytes::borrowed(owned);
}

void HostedInput::pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists)
{
    pack(shape, value, output, lists, std::size_t{0});
}

void HostedInput::pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists,
                       const void* reusableByteSlot)
{
    pack(shape, value, output, lists, 0, reusableByteSlot);
}

void HostedInput::pack(const OwnedShape& shape, const HostedValue& value, std::uint64_t* output, NativeLists* lists,
                       std::size_t depth, const void* reusableByteSlot)
{
    using Kind = HostedValue::Kind;
    if (depth >= 128)
        throw model::ExecutionFailure("native hosted input exceeds the storage depth limit");
    if (shape.words == 0 || shape.words > maximumFrameWords)
        throw model::ExecutionFailure("invalid native hosted value size");

    const auto requireType = [&] {
        if (value.typeName() != shape.name)
            throw model::ExecutionFailure("native hosted input does not match its checked nominal type");
    };
    const auto packField = [&](std::size_t index, const HostedValue& member) {
        if (index >= shape.fieldCount)
            throw model::ExecutionFailure("invalid native hosted field index");
        const auto& field = shape.fields[index];
        if (!field.shape || field.offsetWords >= shape.words || field.shape->words > shape.words - field.offsetWords)
            throw model::ExecutionFailure("invalid native hosted field layout");
        // Failure and cancellation payloads can outlive the next read.
        const auto* byteSlot = shape.kind == OwnedKind::Result && index == 2 ? reusableByteSlot : nullptr;
        pack(*field.shape, member, output + field.offsetWords, lists, depth + 1, byteSlot);
    };

    switch (shape.kind) {
    case OwnedKind::Integer:
        output[0] = static_cast<std::uint64_t>(value.integer());
        return;
    case OwnedKind::UInt32:
        output[0] = value.uint32();
        return;
    case OwnedKind::Float:
        output[0] = std::bit_cast<std::uint64_t>(value.floating());
        return;
    case OwnedKind::Float32:
        output[0] = std::bit_cast<std::uint32_t>(value.float32());
        return;
    case OwnedKind::ByteBuffer: {
        const auto view = retainBytes(value.bytes(), reusableByteSlot);
        std::memcpy(output, &view, sizeof(view));
        return;
    }
    case OwnedKind::Boolean:
        output[0] = value.boolean();
        return;
    case OwnedKind::ContextOwnerReference:
        throw model::ExecutionFailure("a native context owner is not an owned input value");
    case OwnedKind::ObjectReference: {
        requireType();
        const auto& object = value.require(Kind::ObjectReference);
        const auto word = std::bit_cast<std::uint64_t>(object.number);
        if (object.exportedReference) {
            const auto& reference = *static_cast<const HostedValue::ExportedObjectNode&>(object).reference;
            const auto owner = reference.requireOwner();
            if (!domain_ || owner.get() != domain_->exportedOwner)
                throw model::ExecutionFailure("object belongs to a different owner domain");
        }
        if (!domain_ || !domain_->owns(reinterpret_cast<const void*>(word)))
            throw model::ExecutionFailure("native receiver reference does not belong to its graph");
        output[0] = word;
        return;
    }
    case OwnedKind::FixedArray: {
        requireType();
        const auto& items = value.require(Kind::List).children;
        if (items.size() != shape.valueCount)
            throw model::ExecutionFailure("native hosted fixed array has the wrong extent");
        const auto& element = *shape.fields[0].shape;
        std::fill_n(output, shape.words, 0);
        for (std::size_t index = 0; index < items.size(); ++index)
            pack(element, items[index], output + index * element.words, lists, depth + 1);
        if (lists)
            lists->validateFixedArray(shape, output);
        return;
    }
    case OwnedKind::Text:
    case OwnedKind::Error: {
        const auto& source = value.require(shape.kind == OwnedKind::Text ? Kind::Text : Kind::Error).text;
        const auto text = retainText(source);
        std::memcpy(output, &text, sizeof(text));
        return;
    }
    case OwnedKind::Pair:
    case OwnedKind::Record: {
        const auto& record = value.require(Kind::Record);
        requireType();
        if (record.children.size() != shape.fieldCount)
            throw model::ExecutionFailure("native hosted input record has the wrong field count");
        for (std::size_t index = 0; index < shape.fieldCount; ++index)
            packField(index, value.field(shape.fields[index].name));
        return;
    }
    case OwnedKind::Enum: {
        requireType();
        const auto ordinal = static_cast<std::uint64_t>(value.ordinal());
        if (ordinal >= shape.valueCount || value.enumerator() != shape.enumNames[ordinal])
            throw model::ExecutionFailure("native hosted input has an invalid enumerator");
        output[0] = ordinal;
        return;
    }
    case OwnedKind::Optional:
        requireType();
        std::fill_n(output, shape.words, 0);
        if (value.hasValue()) {
            output[0] = 1;
            packField(1, value.value());
        }
        return;
    case OwnedKind::Result:
        requireType();
        std::fill_n(output, shape.words, 0);
        switch (value.outcome()) {
        case model::Outcome::Success:
            output[0] = static_cast<std::uint64_t>(ResultTag::Success);
            if (shape.fieldCount == 3)
                packField(2, value.value());
            else if (value.value().kind() != Kind::Void)
                throw model::ExecutionFailure("native hosted void Result has an unexpected payload");
            return;
        case model::Outcome::Failure:
            output[0] = static_cast<std::uint64_t>(ResultTag::Failure);
            packField(1, value.error());
            return;
        case model::Outcome::Cancelled:
            output[0] = static_cast<std::uint64_t>(ResultTag::Cancelled);
            packField(1, value.error());
            return;
        }
        throw model::ExecutionFailure("native hosted input has an invalid Result outcome");
    case OwnedKind::ResultInterface: {
        requireType();
        const auto& result = value.require(Kind::ResultInterface);
        std::fill_n(output, shape.words, 0);
        if (!result.children.empty()) {
            std::memcpy(output, result.text.data(), sizeof(ResultInterfaceHeader));
            packField(4, result.children.front());
        }
        return;
    }
    case OwnedKind::Dictionary: {
        requireType();
        const auto& entries = value.require(Kind::Dictionary).children;
        if (!lists || shape.fieldCount != 3 || !shape.fields[2].shape ||
            entries.size() > model::maximumListElements)
            throw model::ExecutionFailure("native hosted input has an invalid dictionary");
        const auto& pair = *shape.fields[2].shape;
        if (pair.words == 0 || pair.words > maximumFrameWords)
            throw model::ExecutionFailure("invalid native hosted dictionary entry size");
        const auto view = lists->dictionaries().import(*lists, shape, entries.size(),
            [&](std::size_t index, std::span<std::uint64_t> words) {
                pack(pair, entries[index], words.data(), lists, depth + 1);
            });
        std::memcpy(output, &view, sizeof(view));
        return;
    }
    case OwnedKind::Queue:
    case OwnedKind::Stack: {
        requireType();
        const bool isStack = shape.kind == OwnedKind::Stack;
        const auto& items = value.require(isStack ? Kind::Stack : Kind::Queue).children;
        if (!lists || items.size() > model::maximumListElements || shape.fieldCount != 1 ||
            !shape.fields[0].shape)
            throw model::ExecutionFailure("native hosted input has an invalid sequence");
        const auto& element = *shape.fields[0].shape;
        if (element.words == 0 || element.words > maximumFrameWords)
            throw model::ExecutionFailure("invalid native hosted sequence element size");

        std::vector<std::uint64_t> words(element.words);
        SequenceView view{};
        for (std::size_t index = 0; index < items.size(); ++index) {
            const auto& item = items[isStack ? items.size() - index - 1 : index];
            std::fill(words.begin(), words.end(), 0);
            pack(element, item, words.data(), lists, depth + 1);
            view = lists->appendSequence(view, shape, words.data());
        }
        std::memcpy(output, &view, sizeof(view));
        return;
    }
    case OwnedKind::List:
    case OwnedKind::HashSet: {
        requireType();
        const bool isList = shape.kind == OwnedKind::List;
        const auto& items = value.require(isList ? Kind::List : Kind::HashSet).children;
        if (!lists || items.size() > model::maximumListElements || shape.fieldCount != 1 ||
            !shape.fields[0].shape)
            throw model::ExecutionFailure("native hosted input has an invalid collection");
        const auto& element = *shape.fields[0].shape;
        if (element.words == 0 || element.words > maximumFrameWords)
            throw model::ExecutionFailure("invalid native hosted collection element size");

        if (!isList) {
            const auto set = lists->sets().import(element, items.size(), [&](std::size_t index, std::uint64_t* words) {
                pack(element, items[index], words, lists, depth + 1);
            });
            std::memcpy(output, &set, sizeof(set));
            return;
        }
        std::vector<std::uint64_t> words(element.words);
        ListView list{};
        for (const auto& item : items) {
            std::fill(words.begin(), words.end(), 0);
            pack(element, item, words.data(), lists, depth + 1);
            list = lists->append(list, element, words.data());
        }
        std::memcpy(output, &list, sizeof(list));
        return;
    }
    case OwnedKind::Array: {
        requireType();
        const auto& items = value.require(Kind::List).children;
        if (!lists)
            throw model::ExecutionFailure("native hosted array requires invocation storage");
        const auto& element = *shape.fields[0].shape;
        const auto view = lists->importArray(element, items.size(), [&](std::size_t index, std::uint64_t* item) {
            pack(element, items[index], item, lists, depth + 1);
        });
        std::memcpy(output, &view, sizeof(view));
        return;
    }
    default:
        throw model::ExecutionFailure("native hosted input has an unsupported value layout");
    }
}

namespace {

template<class T>
T read(const std::byte* payload)
{
    T result;
    std::memcpy(&result, payload, sizeof(T));
    return result;
}

model::Outcome projectOutcome(ResultTag tag)
{
    switch (tag) {
    case ResultTag::Success: return model::Outcome::Success;
    case ResultTag::Failure: return model::Outcome::Failure;
    case ResultTag::Cancelled: return model::Outcome::Cancelled;
    default: throw model::ExecutionFailure("native hosted result has an invalid outcome");
    }
}

}

HostedValue HostedProjection::copy(const OwnedShape& shape, const std::uint64_t* payload)
{
    return copy(shape, reinterpret_cast<const std::byte*>(payload), 0);
}

void HostedProjection::charge(std::size_t bytes)
{
    constexpr auto budget = 32 * model::maximumListBytes;
    if (bytes > budget - bytes_)
        throw model::ExecutionFailure("native hosted result exceeds the projection storage limit");
    bytes_ += bytes;
}

void HostedProjection::copyText(std::string& destination, std::string_view source)
{
    charge(source.size());
    destination.assign(source);
}

HostedValue HostedProjection::copy(const OwnedShape& shape, const std::byte* payload, std::size_t depth)
{
    using Kind = HostedValue::Kind;
    if (depth >= 128)
        throw model::ExecutionFailure("native hosted result exceeds the projection depth limit");
    if (!retained_.empty()) {
        const auto* address = reinterpret_cast<const std::uint64_t*>(payload);
        const auto retained = std::lower_bound(retained_.begin(), retained_.end(), address,
            [](const RetainedField& field, const std::uint64_t* source) {
                return std::less<>{}(field.address, source);
            });
        if (retained != retained_.end() && retained->address == address && retained->shape == &shape) {
            charge(retained->bytes);
            return retained->value;
        }
    }
    const bool exportsReference = shape.kind == OwnedKind::ObjectReference && exporter_;
    charge(exportsReference ? sizeof(HostedValue::ExportedObjectNode) + sizeof(ExportedReceiverReference)
                            : sizeof(HostedValue::Node));
    std::shared_ptr<HostedValue::Node> node;
    if (exportsReference)
        node = std::make_shared<HostedValue::ExportedObjectNode>();
    else
        node = std::make_shared<HostedValue::Node>();
    copyText(node->type, shape.name);

    const auto reserve = [&](std::size_t count) {
        charge(count * sizeof(HostedValue));
        node->children.reserve(count);
    };
    const auto field = [&](std::size_t index) {
        const auto& member = shape.fields[index];
        node->children.push_back(copy(*member.shape, payload + member.offsetWords * sizeof(std::uint64_t), depth + 1));
    };

    switch (shape.kind) {
    case OwnedKind::Integer:
        node->kind = Kind::Integer;
        node->number = read<std::int64_t>(payload);
        break;
    case OwnedKind::Float:
        node->kind = Kind::Float;
        node->number = read<std::int64_t>(payload);
        break;
    case OwnedKind::Float32: {
        const auto word = read<std::uint64_t>(payload);
        if (word > std::numeric_limits<std::uint32_t>::max())
            throw model::ExecutionFailure("native hosted result contains invalid float32 storage");
        node->kind = Kind::Float32;
        node->number = static_cast<std::int64_t>(word);
        break;
    }
    case OwnedKind::ByteBuffer:
        node->kind = Kind::ByteBuffer;
        copyText(node->text, NativeBytes::asText(lists_.bytes().view(read<ByteView>(payload))));
        break;
    case OwnedKind::UInt32: {
        const auto word = read<std::uint64_t>(payload);
        if (word > std::numeric_limits<std::uint32_t>::max())
            throw model::ExecutionFailure("native hosted result contains an invalid uint32");
        node->kind = Kind::UInt32;
        node->number = static_cast<std::int64_t>(word);
        break;
    }
    case OwnedKind::Boolean:
        node->kind = Kind::Boolean;
        node->number = read<std::uint64_t>(payload) != 0;
        break;
    case OwnedKind::ContextOwnerReference:
        throw model::ExecutionFailure("use startContextOwner to read a native context owner");
    case OwnedKind::ObjectReference: {
        const auto word = read<std::uint64_t>(payload);
        const auto* reference = reinterpret_cast<const void*>(word);
        if (!domain_ || !domain_->owns(reference))
            throw model::ExecutionFailure("native receiver reference does not belong to its graph");
        charge(sizeof(reference));
        if (exportsReference) {
            auto& exported = static_cast<HostedValue::ExportedObjectNode&>(*node);
            exported.reference = exporter_->exportReference(reference);
            exported.exportedReference = true;
        } else {
            references_.push_back(reference);
        }
        node->kind = Kind::ObjectReference;
        node->number = std::bit_cast<std::int64_t>(word);
        break;
    }
    case OwnedKind::Text:
    case OwnedKind::Error: {
        node->kind = shape.kind == OwnedKind::Text ? Kind::Text : Kind::Error;
        const auto text = read<TextView>(payload);
        if (text.size > model::maximumStringBytes || (text.size && !text.bytes))
            throw model::ExecutionFailure("native hosted result contains invalid text");
        if (text.size)
            copyText(node->text, {text.bytes, text.size});
        model::validate(node->text);
        break;
    }
    case OwnedKind::Enum: {
        node->kind = Kind::Enum;
        const auto ordinal = read<std::uint64_t>(payload);
        if (ordinal >= shape.valueCount)
            throw model::ExecutionFailure("native hosted enumerator is outside its declared range");
        node->number = static_cast<std::int64_t>(ordinal);
        copyText(node->text, shape.enumNames[ordinal]);
        break;
    }
    case OwnedKind::Pair:
    case OwnedKind::Record:
        node->kind = Kind::Record;
        reserve(shape.fieldCount);
        charge(shape.fieldCount * sizeof(std::string));
        node->names.resize(shape.fieldCount);
        for (std::size_t index = 0; index < shape.fieldCount; ++index) {
            copyText(node->names[index], shape.fields[index].name);
            field(index);
        }
        break;
    case OwnedKind::FixedArray: {
        node->kind = Kind::List;
        const auto& element = *shape.fields[0].shape;
        const auto stride = element.words * sizeof(std::uint64_t);
        reserve(shape.valueCount);
        for (std::size_t index = 0; index < shape.valueCount; ++index)
            node->children.push_back(copy(element, payload + index * stride, depth + 1));
        break;
    }
    case OwnedKind::Optional:
        node->kind = Kind::Optional;
        if (read<std::uint64_t>(payload) != 0) {
            reserve(1);
            field(1);
        }
        break;
    case OwnedKind::Result: {
        node->kind = Kind::Result;
        const auto outcome = projectOutcome(read<ResultTag>(payload));
        node->number = static_cast<std::int64_t>(outcome);
        const bool hasPayload = outcome == model::Outcome::Success && shape.fieldCount == 3;
        reserve(hasPayload ? 2 : 1);
        field(1);
        if (hasPayload)
            field(2);
        break;
    }
    case OwnedKind::ResultInterface: {
        node->kind = Kind::ResultInterface;
        const auto header = read<ResultInterfaceHeader>(payload);
        if (header.identity == 0)
            break;
        if (header.ownedBytes > model::maximumListBytes || header.ownedElements > model::maximumListElements)
            throw model::ExecutionFailure("result interface exceeds owned value limits");
        node->number = static_cast<std::int64_t>(projectOutcome(static_cast<ResultTag>(header.outcome)));
        // Keep the ownership stamp and identity while the success payload remains opaque.
        copyText(node->text, {reinterpret_cast<const char*>(&header), sizeof(header)});
        reserve(1);
        field(4);
        break;
    }
    case OwnedKind::Dictionary: {
        node->kind = Kind::Dictionary;
        const auto view = read<DictionaryView>(payload);
        reserve(view.count);
        const auto& pair = *shape.fields[2].shape;
        std::vector<std::uint64_t> words(pair.words);
        for (std::size_t index = 0; index < view.count; ++index) {
            lists_.dictionaries().copyEntry(view, shape, index, words.data());
            node->children.push_back(copy(pair, reinterpret_cast<const std::byte*>(words.data()), depth + 1));
        }
        break;
    }
    case OwnedKind::Queue:
    case OwnedKind::Stack: {
        node->kind = shape.kind == OwnedKind::Stack ? Kind::Stack : Kind::Queue;
        const auto view = read<SequenceView>(payload);
        reserve(view.count);
        const auto& element = *shape.fields[0].shape;
        for (std::size_t index = 0; index < view.count; ++index)
            node->children.push_back(copy(element, lists_.sequenceElement(view, shape, index).data(), depth + 1));
        break;
    }
    case OwnedKind::List: {
        node->kind = Kind::List;
        const auto list = read<ListView>(payload);
        const auto& element = *shape.fields[0].shape;
        const auto content = lists_.borrowPrefix(list, element);
        reserve(list.count);
        const auto stride = element.words * sizeof(std::uint64_t);
        for (std::size_t offset = 0; offset < content.size(); offset += stride)
            node->children.push_back(copy(element, content.data() + offset, depth + 1));
        break;
    }
    case OwnedKind::HashSet: {
        node->kind = Kind::HashSet;
        const auto set = read<SetView>(payload);
        if (set.count > model::maximumListElements)
            throw model::ExecutionFailure("invalid native hosted HashSet count");
        const auto& key = *shape.fields[0].shape;
        reserve(set.count);
        lists_.sets().visitValues(set, key, [&](const void* item) {
            node->children.push_back(copy(key, static_cast<const std::byte*>(item), depth + 1));
        });
        break;
    }
    case OwnedKind::Array: {
        node->kind = Kind::List;
        const auto array = read<ArrayView>(payload);
        const auto& element = *shape.fields[0].shape;
        reserve(array.count);
        for (std::size_t index = 0; index < array.count; ++index)
            node->children.push_back(copy(element, lists_.arrayElement(array, element, index).data(), depth + 1));
        break;
    }
    default:
        throw model::ExecutionFailure("native hosted value has an invalid kind");
    }
    return HostedValue(std::move(node));
}

}
