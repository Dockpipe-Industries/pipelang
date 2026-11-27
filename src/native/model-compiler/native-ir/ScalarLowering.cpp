// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeOptimization.hpp"
#include "ScalarFunction.hpp"
#include "../CompositionGraph.hpp"
#include "../CompositionFactoryProfile.hpp"
#include "../NativeNumericExports.hpp"
#include "../ProgramTypeUsage.hpp"
#include "native_app_abi.hpp"

#include <pipelang/model/result.hpp>
#include <pipelang/model/source_call.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <span>
#include <unordered_set>

namespace pipelang::models::native_ir {
namespace {

bool concurrentSequence(std::string_view name)
{
    return name == "ConcurrentQueue" || name == "ConcurrentStack";
}

bool hasImplicitSharedHandle(const Type& type)
{
    return isSharedHandle(type) && type.name != "ContextOwner";
}

const Type byteBufferType{"ByteBuffer", {}};

bool readsLocal(const Expr& expression, std::string_view name)
{
    return expression && expression->kind == "name" && expression->resolution == "local" &&
        expression->text == name;
}

bool readsLocalResult(const Expr& expression, std::string_view name)
{
    return expression && expression->resolution == "result" && expression->children.size() == 1 &&
        readsLocal(expression->children.front(), name);
}

bool readsLocalBytePayload(const Expr& expression, std::string_view name)
{
    return readsLocalResult(expression, name) && expression->text == "Value" && expression->type == byteBufferType;
}

bool synchronousHostCall(const Expr& expression)
{
    return expression && (expression->resolution == "host_sync" || expression->resolution == "host_sync_value");
}

bool keepsHostBytesLocal(const Expr& expression, std::string_view name)
{
    if (!expression)
        return true;
    if (readsLocal(expression, name))
        return false;
    if (readsLocalResult(expression, name))
        return expression->text == "IsSuccess" || expression->text == "IsFailure" ||
            expression->text == "IsCancelled" || expression->text == "Error";
    if (expression->resolution == "bytebuffer_length" && readsLocalBytePayload(expression->children.at(0), name))
        return true;

    for (std::size_t index = 0; index < expression->children.size(); ++index) {
        const auto& child = expression->children[index];
        // A host call copies its arguments before invoking the service.
        if (index != 0 && synchronousHostCall(expression) && readsLocalBytePayload(child, name))
            continue;
        if (!keepsHostBytesLocal(child, name))
            return false;
    }
    return true;
}

bool keepsHostBytesLocal(std::span<const Statement> statements, std::string_view name)
{
    for (const auto& statement : statements) {
        if (statement.name == name || std::ranges::any_of(statement.parallelCaptures,
                [&](const Field& capture) { return capture.name == name; }))
            return false;
        if (!keepsHostBytesLocal(statement.expression, name) || !keepsHostBytesLocal(statement.target, name))
            return false;
        for (const auto* nested : {&statement.body, &statement.alternative, &statement.cleanup,
                                  &statement.initialization, &statement.iteration}) {
            if (!keepsHostBytesLocal(*nested, name))
                return false;
        }
    }
    return true;
}

ScalarType scalarType(const Type& type, std::size_t offset)
{
    if (type == Type{"int", {}})
        return ScalarType::Integer;
    if (type == Type{"bool", {}})
        return ScalarType::Boolean;

    throw Diagnostic("native scalar backend requires int or bool values", offset);
}

Predicate predicate(std::string_view operation, std::size_t offset)
{
    if (operation == "==")
        return Predicate::Equal;
    if (operation == "!=")
        return Predicate::NotEqual;
    if (operation == "<")
        return Predicate::Less;
    if (operation == "<=")
        return Predicate::LessEqual;
    if (operation == ">")
        return Predicate::Greater;
    if (operation == ">=")
        return Predicate::GreaterEqual;

    throw Diagnostic("native scalar backend supports comparison operators only", offset);
}

using Bindings = std::map<std::string, std::size_t>;
const Type integerResult{"Result", {Type{"int", {}}, Type{"string", {}}}};
const Type voidResult{"Result", {Type{"void", {}}, Type{"string", {}}}};
const Type errorValue{"Error", {}};
const Type cancellationToken{"CancellationToken", {}};
const Type processArgumentsType{"Array", {Type{"string", {}}}};
const Type voidType{"void", {}};

enum class ReceiverMode { Ordinary, QualifiedPure, ParallelWorker };

bool directSynchronousCall(const Expr& call)
{
    return call->kind == "call" && (call->resolution == "static_call" || call->resolution == "object_call" ||
           call->resolution == "pure" || call->resolution == "effect" || call->resolution == "qualified_pure");
}

bool receiverFreeWorkerCall(const Expr& call)
{
    return call->statelessWorkerCall || call->resolution == "pure" || call->resolution == "effect";
}

bool hashSetCall(const Expr& expression)
{
    return expression->kind == "call" && expression->children.at(0)->kind == "member" &&
           expression->children.at(0)->children.at(0)->type.name == "HashSet" &&
           (expression->resolution.starts_with("collection_read_") ||
            expression->resolution.starts_with("collection_mutate_"));
}

bool sequenceType(const Type& type)
{
    return type.name == "Queue" || type.name == "Stack";
}

bool sequenceCallExpression(const Expr& expression)
{
    return expression->kind == "call" && expression->children.front()->kind == "member" &&
        sequenceType(expression->children.front()->children.front()->type) &&
        (expression->resolution.starts_with("collection_read_") ||
         expression->resolution.starts_with("collection_mutate_"));
}

bool dictionaryCallExpression(const Expr& expression)
{
    return expression->kind == "call" && expression->children.front()->kind == "member" &&
        expression->children.front()->children.front()->type.name == "Dictionary" &&
        (expression->resolution.starts_with("collection_read_") ||
         expression->resolution.starts_with("collection_mutate_"));
}

class ValueLayouts {
public:
    explicit ValueLayouts(const std::vector<Declaration>& declarations) : declarations_(declarations) {}

    NativeType type(const Type& source, std::size_t offset)
    {
        if (source == Type{"int", {}})
            return ScalarType::Integer;
        if (source == Type{"uint32", {}})
            return ScalarType::UInt32;
        if (source == Type{"float", {}})
            return ScalarType::Float;
        if (source == Type{"float32", {}})
            return ScalarType::Float32;
        if (source == byteBufferType)
            return ScalarType::ByteBuffer;
        if (source == Type{"bool", {}})
            return ScalarType::Boolean;
        if (source == Type{"string", {}})
            return ScalarType::Text;
        if (source == errorValue)
            return ScalarType::Error;
        if (source == Type{"SemaphoreSlim", {}})
            return ScalarType::Semaphore;
        if (source == Type{"Lock", {}})
            return ScalarType::LockReference;
        if (source == Type{"ContextOwner", {}})
            return ScalarType::ContextOwnerReference;
        if (sourceInterface(source))
            return ScalarType::Interface;

        const auto existing = std::find_if(types_.begin(), types_.end(), [&](const auto& known) {
            return known.first == source;
        });
        if (existing != types_.end())
            return existing->second;

        if (source.name == "ConcurrentDictionary" && source.arguments.size() == 2) {
            const auto key = type(source.arguments[0], offset);
            const auto value = type(source.arguments[1], offset);
            if (!persistentValue(value, true))
                throw Diagnostic("native concurrent dictionaries require owned values", offset);
            return add(source, ScalarType::ConcurrentDictionaryReference,
                       "ConcurrentDictionary<" + typeName(key) + ',' + typeName(value) + '>',
                       {{"Key", key}, {"Value", value}}, offset);
        }

        if (concurrentSequence(source.name) && source.arguments.size() == 1) {
            const auto element = type(source.arguments.front(), offset);
            if (!persistentValue(element, true))
                throw Diagnostic("native concurrent sequences require owned elements", offset);
            return add(source, ScalarType::ConcurrentSequenceReference, source.name + '<' + typeName(element) + '>',
                       {{"element", element}}, offset);
        }

        if (source.name == "Atomic" && source.arguments.size() == 1) {
            const auto value = type(source.arguments.front(), offset);
            return add(source, ScalarType::AtomicReference, "Atomic<" + typeName(value) + '>',
                       {{"value", value}}, offset);
        }

        if (source.name == "FixedArray" && source.fixedExtent) {
            const auto element = type(source.arguments.at(0), offset);
            const auto extent = *source.fixedExtent;
            const auto elementWords = valueWords(element, layouts_);
            if (extent > native_app_v19::maximumFrameWords / elementWords)
                throw Diagnostic("native fixed array exceeds the value storage limit", offset);
            const NativeType result{ScalarType::FixedArray, layouts_.size()};
            layouts_.push_back({result.kind, "FixedArray<" + typeName(element) + ',' + std::to_string(extent) + '>',
                                {{"element", element}}, std::max(std::size_t{1}, extent * elementWords), {}, extent});
            types_.emplace_back(source, result);
            return result;
        }

        if ((source.name == "List" || source.name == "Array" || source.name == "HashSet" || sequenceType(source)) &&
            source.arguments.size() == 1) {
            const auto element = type(source.arguments[0], offset);
            auto collection = ScalarType::List;
            if (source.name == "Array")
                collection = ScalarType::Array;
            else if (source.name == "HashSet")
                collection = ScalarType::HashSet;
            else if (source.name == "Queue")
                collection = ScalarType::Queue;
            else if (source.name == "Stack")
                collection = ScalarType::Stack;
            const NativeType result{collection, layouts_.size()};
            const std::size_t words = isSequence(collection) ? 3 : 2;
            layouts_.push_back({collection, source.name + '<' + typeName(element) + '>', {{"element", element}}, words});
            types_.emplace_back(source, result);
            return result;
        }

        if ((source.name == "Dictionary" || source.name == "KeyValuePair") && source.arguments.size() == 2) {
            const auto key = type(source.arguments[0], offset);
            const auto mapped = type(source.arguments[1], offset);
            const auto name = source.name + '<' + typeName(key) + ',' + typeName(mapped) + '>';
            std::vector<ValueField> fields{{"key", key}, {"value", mapped}};
            if (source.name == "KeyValuePair")
                return add(source, ScalarType::Pair, name, std::move(fields), offset);
            const auto entry = type(Type{"KeyValuePair", source.arguments}, offset);
            fields.push_back({"entry", entry});
            const NativeType result{ScalarType::Dictionary, layouts_.size()};
            layouts_.push_back({result.kind, name, std::move(fields), 2});
            types_.emplace_back(source, result);
            return result;
        }

        if (source.name == "IResult" && source.arguments.size() == 1) {
            const auto error = type(source.arguments.front(), offset);
            return add(source, ScalarType::ResultInterface, "IResult<" + typeName(error) + '>',
                       {{"identity", ScalarType::Integer}, {"outcome", ScalarType::Integer},
                        {"ownedBytes", ScalarType::Integer}, {"ownedElements", ScalarType::Integer},
                        {"error", error}}, offset);
        }

        if (source.name == "Result" && source.arguments.size() == 2) {
            const auto error = type(source.arguments[1], offset);
            std::vector<ValueField> fields{{"tag", ScalarType::Integer}, {"error", error}};
            auto payloadName = std::string{"void"};
            if (source.arguments[0] != Type{"void", {}}) {
                const auto payload = type(source.arguments[0], offset);
                payloadName = typeName(payload);
                fields.push_back({"value", payload});
            }
            return add(source, ScalarType::Result, "Result<" + payloadName + ',' + typeName(error) + '>',
                       std::move(fields), offset);
        }

        if (source.name == "Optional" && source.arguments.size() == 1) {
            const auto payload = type(source.arguments[0], offset);
            return add(source, ScalarType::Optional, "Optional<" + typeName(payload) + ">",
                       {{"hasValue", ScalarType::Boolean}, {"value", payload}}, offset);
        }

        const auto* record = declaration(source);
        if (sourceReceiver(source) && source.arguments.empty())
            return add(source, ScalarType::ClassReference, record->sourceName, {}, offset);
        if (record && record->kind == "Enum" && source.arguments.empty()) {
            const NativeType result{ScalarType::Enum, layouts_.size()};
            layouts_.push_back({ScalarType::Enum, record->sourceName, {}, 1, record->values});
            types_.emplace_back(source, result);
            return result;
        }
        if (!record || record->kind != "Record" || !source.arguments.empty() ||
            !record->typeParameters.empty())
            throw Diagnostic("native value layout does not support " + (record ? record->sourceName : source.name), offset);
        if (!activeRecords_.insert(record->name).second)
            throw Diagnostic("native value layout cannot contain a recursive inline record", offset);

        std::vector<ValueField> fields;
        for (const auto& field : record->fields)
            fields.push_back({field.name, type(field.type, offset)});
        activeRecords_.erase(record->name);
        return add(source, ScalarType::Record, record->sourceName, std::move(fields), offset);
    }

    const ValueLayout& operator[](NativeType type) const { return layouts_.at(type.layout); }
    std::span<const ValueLayout> view() const { return layouts_; }
    std::vector<ValueLayout> take() { return std::move(layouts_); }

    const Declaration* declaration(const Type& source) const
    {
        const auto found = std::find_if(declarations_.begin(), declarations_.end(), [&](const auto& candidate) {
            return candidate.name == source.name;
        });
        return found == declarations_.end() ? nullptr : &*found;
    }

    bool service(const Type& source) const
    {
        const auto* type = declaration(source);
        return type && type->kind == "Interface" && !type->sourceInterface;
    }

    bool sourceReceiver(const Type& source) const
    {
        const auto* value = declaration(source);
        return value && value->kind == "Class" && !value->sourceInterface;
    }

    bool sourceInterface(const Type& source) const
    {
        const auto* value = declaration(source);
        return value && value->sourceInterface;
    }

    std::size_t receiverTag(const Declaration& owner) const
    {
        const bool usesDispatch = !owner.interfaces.empty() || !owner.baseClass.empty() ||
            std::any_of(owner.methods.begin(), owner.methods.end(), [](const auto& method) {
                return method.virtualMethod;
            });
        return usesDispatch ? static_cast<std::size_t>(&owner - declarations_.data()) + 1 : 0;
    }

    bool receiverUpcast(const Type& source, const Type& target) const
    {
        const auto* owner = declaration(source);
        return owner && std::find(owner->ancestors.begin(), owner->ancestors.end(), target.name) != owner->ancestors.end();
    }

    bool containsReference(NativeType value, bool allowsObjectReferences = false) const
    {
        if (value == ScalarType::Interface || value == ScalarType::ClassReference)
            return !allowsObjectReferences;
        if (value == ScalarType::Semaphore ||
            value == ScalarType::AtomicReference || value == ScalarType::LockReference ||
            value == ScalarType::ConcurrentSequenceReference || value == ScalarType::ConcurrentDictionaryReference ||
            value == ScalarType::ContextOwnerReference)
            return true;
        if (!usesAggregateLayout(value))
            return false;
        const auto& fields = (*this)[value].fields;
        return std::any_of(fields.begin(), fields.end(),
                           [&](const auto& field) { return containsReference(field.type, allowsObjectReferences); });
    }

    bool containsErasedResult(NativeType value) const
    {
        if (value == ScalarType::ResultInterface)
            return true;
        if (!usesAggregateLayout(value))
            return false;
        const auto& fields = (*this)[value].fields;
        return std::any_of(fields.begin(), fields.end(),
                           [&](const auto& field) { return containsErasedResult(field.type); });
    }

    bool persistentValue(NativeType value, bool sourceOnly = false) const
    {
        if (isReceiverValue(value))
            return !containsReference(value);
        switch (value.kind) {
        case ScalarType::Integer:
        case ScalarType::UInt32:
        case ScalarType::Float:
        case ScalarType::Float32:
        case ScalarType::ByteBuffer:
        case ScalarType::Boolean:
        case ScalarType::Text:
            return true;
        case ScalarType::HashSet:
            return !containsReference(value);
        case ScalarType::FixedArray:
            return fixedArrayHasStaticOwnership(value, view()) ||
                (!containsReference(value) && persistentValue((*this)[value].fields.front().type, sourceOnly));
        case ScalarType::Enum:
            return sourceOnly;
        case ScalarType::Record:
            return std::all_of((*this)[value].fields.begin(), (*this)[value].fields.end(),
                               [&](const auto& field) { return persistentValue(field.type, sourceOnly); });
        default:
            return false;
        }
    }

    bool invocationField(NativeType value) const
    {
        if (value == ScalarType::Record) {
            const auto& fields = (*this)[value].fields;
            return std::all_of(fields.begin(), fields.end(), [&](const auto& field) {
                return persistentValue(field.type, true) || invocationField(field.type);
            });
        }
        return value == ScalarType::Semaphore || value == ScalarType::AtomicReference ||
            value == ScalarType::LockReference || value == ScalarType::ConcurrentSequenceReference ||
            value == ScalarType::ConcurrentDictionaryReference || value == ScalarType::Optional ||
            value == ScalarType::List || value == ScalarType::Dictionary ||
            value == ScalarType::FixedArray || value == ScalarType::Array || isSequence(value.kind);
    }

    NativeType receiverField(const Type& source, std::size_t offset)
    {
        if (service(source))
            return ScalarType::Service;
        if (sourceReceiver(source))
            return ScalarType::Receiver;
        return type(source, offset);
    }

    std::size_t receiverOffset(const Declaration& owner, std::size_t index)
    {
        std::size_t offset = 0;
        if (!owner.baseClass.empty()) {
            const auto* base = declaration(Type{owner.baseClass, {}});
            if (!base)
                throw std::logic_error("native layout lost its checked base class");
            offset = receiverOffset(*base, base->fields.size());
        }
        for (const auto& field : std::span(owner.fields).first(index)) {
            const auto fieldType = receiverField(field.type, field.offset);
            offset += valueWords(fieldType, view());
        }
        return offset;
    }

    std::string typeName(NativeType type) const
    {
        switch (type.kind) {
        case ScalarType::Integer: return "int";
        case ScalarType::UInt32: return "uint32";
        case ScalarType::Float: return "float";
        case ScalarType::Float32: return "float32";
        case ScalarType::ByteBuffer: return "ByteBuffer";
        case ScalarType::Boolean: return "bool";
        case ScalarType::Text: return "string";
        case ScalarType::Semaphore: return "SemaphoreSlim";
        case ScalarType::LockReference: return "Lock";
        case ScalarType::ContextOwnerReference: return "ContextOwner";
        case ScalarType::Error: return "Error";
        case ScalarType::Interface: return "source interface";
        case ScalarType::Result:
        case ScalarType::ResultInterface:
        case ScalarType::List:
        case ScalarType::Enum:
        case ScalarType::Optional:
        case ScalarType::HashSet:
        case ScalarType::Queue:
        case ScalarType::Stack:
        case ScalarType::Dictionary:
        case ScalarType::Pair:
        case ScalarType::ClassReference:
        case ScalarType::ConcurrentSequenceReference:
        case ScalarType::ConcurrentDictionaryReference:
        case ScalarType::AtomicReference:
        case ScalarType::FixedArray:
        case ScalarType::Array:
        case ScalarType::Record: return layouts_.at(type.layout).name;
        default: throw std::logic_error("invalid inline native value type");
        }
    }

private:
    NativeType add(const Type& source, ScalarType kind, std::string name,
                   std::vector<ValueField> fields, std::size_t offset)
    {
        std::size_t words = 0;
        for (const auto& field : fields) {
            const auto size = valueWords(field.type, layouts_);
            if (size > native_app_v19::maximumFrameWords - words)
                throw Diagnostic("native value exceeds the coroutine storage limit", offset);
            words += size;
        }
        const NativeType result{kind, layouts_.size()};
        layouts_.push_back({kind, std::move(name), std::move(fields), std::max(std::size_t{1}, words)});
        types_.emplace_back(source, result);
        return result;
    }

    const std::vector<Declaration>& declarations_;
    std::vector<std::pair<Type, NativeType>> types_;
    std::vector<ValueLayout> layouts_;
    std::set<std::string> activeRecords_;
};

[[noreturn]] void failAtOwner(const Declaration& owner, std::string message, std::size_t offset)
{
    Diagnostic diagnostic(std::move(message), offset);
    diagnostic.module = owner.module;
    diagnostic.path = owner.originPath;
    throw diagnostic;
}

std::int64_t decimalLiteral(const Expr& expression)
{
    const auto& text = expression->text;
    std::int64_t integer = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
    if (expression->kind != "integer" || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        throw Diagnostic("native backend requires a signed decimal literal", expression->offset);

    return integer;
}

std::optional<IntegerOperation> integerOperation(std::string_view operation)
{
    if (operation == "+")
        return IntegerOperation::Add;
    if (operation == "-")
        return IntegerOperation::Subtract;
    if (operation == "*")
        return IntegerOperation::Multiply;
    if (operation == "/")
        return IntegerOperation::Divide;
    if (operation == "%")
        return IntegerOperation::Remainder;
    return std::nullopt;
}

std::optional<UInt32Operation> uint32Operation(std::string_view spelling)
{
    using Op = UInt32Operation;
    static constexpr std::array operations{
        std::pair{"+", Op::Add}, std::pair{"-", Op::Subtract}, std::pair{"*", Op::Multiply},
        std::pair{"/", Op::Divide}, std::pair{"%", Op::Remainder}, std::pair{"<<", Op::ShiftLeft},
        std::pair{">>", Op::ShiftRight}, std::pair{"&", Op::And}, std::pair{"|", Op::Or},
        std::pair{"^", Op::Xor}, std::pair{"WrappingAdd", Op::WrappingAdd},
        std::pair{"WrappingSubtract", Op::WrappingSubtract}, std::pair{"WrappingMultiply", Op::WrappingMultiply}
    };
    const auto found = std::find_if(operations.begin(), operations.end(), [&](const auto& entry) {
        return entry.first == spelling;
    });
    if (found == operations.end())
        return std::nullopt;
    return found->second;
}

std::optional<FloatingArithmetic> floatingArithmetic(std::string_view spelling)
{
    if (spelling == "+")
        return FloatingArithmetic::Add;
    if (spelling == "-")
        return FloatingArithmetic::Subtract;
    if (spelling == "*")
        return FloatingArithmetic::Multiply;
    if (spelling == "/")
        return FloatingArithmetic::Divide;
    return std::nullopt;
}

native_app_v19::FloatingBuiltin floatingMath(std::string_view spelling, std::size_t arguments)
{
    using Op = native_app_v19::FloatingBuiltin;
    if (spelling == "Hypot" && arguments == 3)
        return Op::Hypot3;
    static constexpr std::array operations{
        std::pair{"IsFinite", Op::IsFinite}, std::pair{"Abs", Op::Abs}, std::pair{"Sqrt", Op::Sqrt},
        std::pair{"Log", Op::Log}, std::pair{"Sin", Op::Sin}, std::pair{"Cos", Op::Cos},
        std::pair{"Tan", Op::Tan}, std::pair{"Asin", Op::Asin}, std::pair{"Acos", Op::Acos},
        std::pair{"Atan", Op::Atan}, std::pair{"Atan2", Op::Atan2}, std::pair{"Exp", Op::Exp},
        std::pair{"Expm1", Op::Expm1}, std::pair{"Pow", Op::Pow}, std::pair{"Hypot", Op::Hypot},
        std::pair{"Fmod", Op::Fmod}, std::pair{"Remainder", Op::Remainder}, std::pair{"CopySign", Op::CopySign},
        std::pair{"Cbrt", Op::Cbrt}, std::pair{"Tanh", Op::Tanh}, std::pair{"Floor", Op::Floor},
        std::pair{"Ceiling", Op::Ceiling}, std::pair{"Clamp", Op::Clamp}, std::pair{"ILogB", Op::ILogB},
        std::pair{"ScaleB", Op::ScaleB}
    };
    const auto found = std::find_if(operations.begin(), operations.end(), [&](const auto& entry) {
        return entry.first == spelling;
    });
    if (found == operations.end())
        throw std::logic_error("unknown checked floating operation");
    return found->second;
}

Operand floatingLiteral(const Expr& expression, NativeType type)
{
    const auto literal = expression->resolution == "ieee_constant" ? foldConstant(expression) : expression;
    const auto& checked = literal->resolution;
    std::uint64_t bits;
    const auto parsed = std::from_chars(checked.data(), checked.data() + checked.size(), bits);
    if (parsed.ec != std::errc{} || parsed.ptr != checked.data() + checked.size())
        throw std::logic_error("checked floating expression is missing its normalized bit pattern");
    return {type, std::bit_cast<std::int64_t>(bits)};
}

struct WorkerField {
    const Field* declaration;
    std::size_t receiverOffset;
};

std::string workerFieldParameter(std::string_view name)
{
    return "$field." + std::string(name);
}

struct ResolvedCall {
    std::string symbol;
    std::size_t unmeteredWork;
    bool retainsReceiver;
    std::vector<WorkerField> workerFields;
};

struct ParallelWorker {
    std::string symbol;
    std::vector<WorkerField> fields;
};

using ResolveCall = std::function<ResolvedCall(const Expr&)>;
using ResolveObject = std::function<const Declaration&(std::string_view, std::size_t)>;
using DispatchTargets = std::vector<std::pair<std::size_t, std::string>>;
using ResolveDispatch = std::function<DispatchTargets(const Expr&)>;
using ResolveParallel = std::function<ParallelWorker(const Statement&)>;

// Only direct receiver operations can use a cell whose slot is reused.
void collectEscapingAtomics(const Expr& expression, std::unordered_set<std::string>& names,
                            const Expression* directReceiver = nullptr)
{
    if (!expression)
        return;
    if (expression->resolution == "local" && expression->type.name == "Atomic" &&
        expression.get() != directReceiver)
        names.insert(expression->text);

    if (expression->resolution.starts_with("atomic_") && expression->resolution != "atomic_construct")
        directReceiver = expression->children.at(0)->children.at(0).get();
    for (const auto& child : expression->children)
        collectEscapingAtomics(child, names, directReceiver);
}

using AtomicAliases = std::map<std::string, std::vector<std::string>>;

void collectAtomicUses(std::span<const Statement> statements, std::unordered_set<std::string>& escaping,
                       AtomicAliases& origins)
{
    for (const auto& statement : statements) {
        const auto& initial = statement.expression;
        if (statement.kind == "local" && statement.type.name == "Atomic" && initial->resolution == "local") {
            // A new local has a lifetime contained by the local it copies.
            // Assigning an existing binding can escape that lifetime.
            origins[statement.name].push_back(initial->text);
        } else {
            collectEscapingAtomics(initial, escaping);
        }
        collectEscapingAtomics(statement.target, escaping);
        for (const auto& capture : statement.parallelCaptures) {
            if (capture.type.name == "Atomic")
                escaping.insert(capture.name);
        }

        collectAtomicUses(statement.initialization, escaping, origins);
        collectAtomicUses(statement.iteration, escaping, origins);
        collectAtomicUses(statement.body, escaping, origins);
        collectAtomicUses(statement.alternative, escaping, origins);
        collectAtomicUses(statement.cleanup, escaping, origins);
    }
}

std::unordered_set<std::string> escapingAtomics(std::span<const Statement> statements)
{
    AtomicAliases origins;
    std::unordered_set<std::string> escaping;
    collectAtomicUses(statements, escaping, origins);

    std::vector<std::string> pending(escaping.begin(), escaping.end());
    while (!pending.empty()) {
        const auto alias = std::move(pending.back());
        pending.pop_back();
        const auto sources = origins.find(alias);
        if (sources == origins.end())
            continue;
        for (const auto& source : sources->second) {
            if (escaping.insert(source).second)
                pending.push_back(source);
        }
    }
    return escaping;
}

enum class Binary32Checks { None, HostValue, EntryAndHostValue };

class ScalarLowering final {
public:
    ScalarLowering(const Declaration& owner, const Method& method, std::string name, std::string symbol,
                   FunctionMode mode, bool exported, bool hostedEntry, const ResolveCall& resolve, const ResolveObject& object,
                   const ResolveDispatch& dispatch, const ResolveParallel& parallel, ValueLayouts& layouts,
                   ReceiverMode receiverMode, bool hostedObjectReferences, bool hostedBindingConstructor,
                   bool preserveSynchronousResult, bool ordinaryModelCancellation, Binary32Checks binary32Checks)
    try
        : function_{std::move(name), std::move(symbol), owner.originPath, method.offset,
                    method.parameters.size() + (!method.staticMethod && receiverMode == ReceiverMode::Ordinary ? 1 : 0),
                    mode, exported, method.constructor || method.result == voidType, {}, {{}}},
          resolve_(resolve), object_(object), dispatch_(dispatch), parallel_(parallel), layouts_(layouts),
          hasReceiver_(!method.staticMethod && receiverMode == ReceiverMode::Ordinary), receiverMode_(receiverMode),
          hostedBindingConstructor_(hostedBindingConstructor), binary32Checks_(binary32Checks)
    {
        function_.checksInvocationCancellation = hostedEntry && !method.asynchronous;
        function_.checksPortableBinary32OnPoll = hostedEntry && method.asynchronous &&
            binary32Checks_ == Binary32Checks::EntryAndHostValue;
        if (function_.continuation())
            escapingAtomics_ = escapingAtomics(method.body);
        const bool synchronousLauncherResult = exported && function_.continuation() &&
            !method.asynchronous && (method.result == Type{"int", {}} || method.result == voidType);
        const bool adaptsResult = hostedEntry || synchronousLauncherResult;
        if (mode == FunctionMode::Scalar && !method.constructor && method.result != Type{"int", {}})
            throw Diagnostic("native scalar entry must return int", method.offset);
        if (function_.continuation()) {
            const auto& result = method.asynchronous ? method.result.arguments.at(0) : method.result;
            if (!function_.returnsVoid)
                function_.result = layouts_.type(result, method.offset);
            if ((method.asynchronous || !adaptsResult) && function_.result != ScalarType::Result)
                throw Diagnostic("native async methods must return Task<Result<T,string>>", method.offset);
            const bool hostedContextGetter = hostedEntry && !method.asynchronous && method.pure &&
                method.parameters.empty() && function_.result == ScalarType::ContextOwnerReference;
            if (exported && !hostedContextGetter && !function_.returnsVoid &&
                layouts_.containsReference(function_.result, hostedObjectReferences))
                throw Diagnostic("native invocation-owned references cannot escape their invocation", method.offset);
        } else if ((function_.usesRuntime() || function_.construction()) && !function_.returnsVoid) {
            function_.result = layouts_.type(method.result, method.offset);
        }

        Bindings bindings;
        if (hasReceiver_)
            function_.slots.push_back({"this", ScalarType::Receiver, method.offset});

        if (function_.usesRuntime() && method.parameters.size() > native_app_v19::maximumArguments)
            throw Diagnostic("native argument count exceeds the ABI limit", method.offset);
        for (const auto& parameter : method.parameters) {
            NativeType type = ScalarType::Integer;
            if (method.constructor && layouts_.service(parameter.type)) {
                type = ScalarType::Service;
            } else if (function_.construction() && layouts_.sourceReceiver(parameter.type)) {
                object_(parameter.type.name, parameter.offset);
                type = ScalarType::Receiver;
            } else if ((function_.usesRuntime() || function_.construction()) && parameter.type == cancellationToken) {
                type = ScalarType::CancellationToken;
            } else if (function_.construction()) {
                type = layouts_.type(parameter.type, parameter.offset);
                if (type != ScalarType::Interface && !hasImplicitSharedHandle(parameter.type) && !layouts_.persistentValue(type, true) &&
                    !(mode == FunctionMode::InvocationConstruction && layouts_.invocationField(type)))
                    throw Diagnostic("native constructor parameter has no storage for its lifetime", parameter.offset);
            } else if (mode == FunctionMode::Contextual) {
                type = layouts_.type(parameter.type, parameter.offset);
            } else if (function_.continuation() && (!exported || hostedEntry || parameter.type == processArgumentsType)) {
                type = layouts_.type(parameter.type, parameter.offset);
                if (hostedEntry && !hasImplicitSharedHandle(parameter.type) && layouts_.containsReference(type, hostedObjectReferences))
                    throw Diagnostic("native hosted arguments cannot contain invocation-owned references",
                                     parameter.offset);
            } else if (parameter.type != Type{"int", {}}) {
                throw Diagnostic(function_.continuation() ? "native async parameters must be int or CancellationToken"
                                                          : "native scalar parameters must be int", parameter.offset);
            }

            bindings.emplace(parameter.name, function_.slots.size());
            if (ordinaryModelCancellation && type == ScalarType::CancellationToken)
                parallelCancellationSlots_.push_back(function_.slots.size());
            function_.slots.push_back({parameter.name, type, parameter.offset});
        }

        std::size_t initialBlock = 0;
        if (binary32Checks_ == Binary32Checks::EntryAndHostValue)
            requireBinary32Environment(initialBlock, method.offset);
        budgetStep(initialBlock, method.offset);
        if (method.constructor) {
            const auto self = load(0, method.offset, initialBlock);
            if (function_.construction() || function_.usesRuntime()) {
                for (std::size_t index = 0; index < owner.fields.size(); ++index) {
                    const auto& member = owner.fields[index];
                    if (!needsMaterializedDefault(member.type))
                        continue;
                    const auto initial = defaultValue(member.type, member.offset, initialBlock);
                    function_.blocks[initialBlock].instructions.push_back(
                        {member.offset, WriteField{self, layouts_.receiverOffset(owner, index), initial, true}});
                }
            }
            if (method.baseConstructor) {
                const auto& constructor = method.baseConstructor;
                const bool runtime = function_.construction() || function_.usesRuntime();
                auto arguments = evaluateOperands(std::span(constructor->children).subspan(1), bindings,
                                                  initialBlock, self, runtime);
                const auto destination = resolve_(constructor);
                budgetStep(initialBlock, constructor->offset, destination.unmeteredWork);
                function_.blocks[initialBlock].instructions.push_back(
                    {constructor->offset, DirectCall{{}, destination.symbol, std::move(arguments),
                                                    runtime ? CallAbi::Construction : CallAbi::Scalar}});
            }
            for (std::size_t index = 0; index < method.fieldInitializers.size(); ++index) {
                const auto& initializer = method.fieldInitializers[index];
                const auto& member = owner.fields[index];
                const bool initializeHandle = (function_.construction() || function_.usesRuntime()) &&
                    hasImplicitSharedHandle(member.type);
                if (!initializer && !initializeHandle)
                    continue;

                const auto field = layouts_.receiverOffset(owner, index);
                if (initializer) {
                    const auto initial = value(initializer, bindings, initialBlock);
                    function_.blocks[initialBlock].instructions.push_back(
                        {initializer->offset, WriteField{self, field, initial, true}});
                } else {
                    // A base constructor may already have reached this cell through virtual dispatch.
                    const auto type = layouts_.type(member.type, member.offset);
                    (void)readReceiverField(self, field, type, member.offset, initialBlock);
                }
            }
        }
        if (const auto exit = statements(method.body, bindings, initialBlock)) {
            if (!function_.returnsVoid)
                throw Diagnostic("native scalar entry must return a value on every path", method.offset);
            terminate(*exit, method.offset, Return{});
        }
        if (function_.construction() && method.constructor) {
            for (std::size_t block = 0; block < function_.blocks.size(); ++block) {
                const auto& ending = function_.blocks[block].exit;
                if (ending && std::holds_alternative<Return>(*ending))
                    validateConstructedReceiver(owner, block);
            }
        }
        if (adaptsResult) {
            const auto& result = method.asynchronous ? method.result.arguments.at(0) : method.result;
            const bool preserveAuthoredOutcome = preserveSynchronousResult && !method.asynchronous;
            if (preserveAuthoredOutcome || result.name != "Result" || result.arguments.at(1) != Type{"string", {}})
                wrapEntryResult(result);
        }
        layoutReceiverStorage();
        if (function_.usesRuntime() && slotStorageWords(function_, layouts_.view()) +
            function_.receiverStorageWords +
            valueWords(function_.result, layouts_.view()) + function_.childResultWords > native_app_v19::maximumFrameWords)
            throw Diagnostic("native function frame exceeds the storage limit", method.offset);
    } catch (Diagnostic& diagnostic) {
        if (diagnostic.path.empty()) {
            diagnostic.module = owner.module;
            diagnostic.path = owner.originPath;
        }
        throw;
    }

    ScalarFunction finish()
    {
        return std::move(function_);
    }

    bool retainsReceiver() const { return retainsReceiver_; }
    bool assignsReceiverReference() const { return assignsReceiverReference_; }

private:
    void validateConstructedReceiver(const Declaration& owner, std::size_t block)
    {
        const auto self = load(0, function_.offset, block);
        for (std::size_t index = 0; index < owner.fields.size(); ++index) {
            const auto& field = owner.fields[index];
            if (layouts_.service(field.type))
                continue;
            const bool reference = layouts_.sourceReceiver(field.type) || layouts_.sourceInterface(field.type);
            const auto type = reference ? NativeType{ScalarType::Receiver} : layouts_.type(field.type, field.offset);
            const bool mustValidate = layouts_.containsErasedResult(type);
            if (type != ScalarType::Receiver && type != ScalarType::ContextOwnerReference && !mustValidate)
                continue;
            const Register value{registers_++};
            auto& instructions = function_.blocks[block].instructions;
            instructions.push_back({field.offset, ReadField{value, self, layouts_.receiverOffset(owner, index), type}});
            if (mustValidate)
                instructions.push_back({field.offset, ValidateConstructedValue{{type, value}}});
        }
    }

    void wrapEntryResult(const Type& sourceResult)
    {
        const auto transport = layouts_.type(Type{"Result", {sourceResult, Type{"string", {}}}}, function_.offset);
        const Operand noError{ScalarType::Text, ZeroValue{}};
        for (std::size_t index = 0; index < function_.blocks.size(); ++index) {
            auto& block = function_.blocks[index];
            if (!block.exit)
                continue;
            auto* returned = std::get_if<Return>(&*block.exit);
            if (returned)
                returned->value = makeResult(transport, ResultKind::Success, returned->value, noError,
                                             block.exitOffset, index);
        }
        function_.result = transport;
    }

    struct ReceiverAllocation {
        std::size_t block;
        std::size_t instruction;
    };

    void layoutReceiverStorage()
    {
        for (auto& block : function_.blocks) {
            for (auto& instruction : block.instructions) {
                auto* receiver = std::get_if<AllocateObject>(&instruction.operation);
                if (!receiver || receiver->arena)
                    continue;
                receiver->frameOffset = function_.receiverStorageWords;
                function_.receiverStorageWords += native_app_v19::receiverHeaderWords + receiver->words;
            }
        }
    }

    struct LoopTargets {
        std::size_t exit;
        std::size_t repeat;
        std::size_t cleanupDepth;
        bool hasBreak = false;
    };

    struct Transfer {
        std::size_t destination;
        std::size_t cleanupDepth;
        bool operator==(const Transfer&) const = default;
    };

    struct ProtectedRegion {
        std::size_t entry;
        std::size_t selector;
        std::vector<Transfer> transfers;
    };

    Operand load(std::size_t index, std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, Load{result, index}});
        return {function_.slots.at(index).type, result};
    }

    static bool receiverField(const Expr& expression)
    {
        return expression->resolution == "object_field" || expression->resolution == "field";
    }

    Operand readReceiverField(Operand object, std::size_t field, NativeType type,
                              std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ReadField{result, object, field, type}});
        return {type, result};
    }

    Operand readField(const Expr& expression, NativeType type, const Bindings& bindings, std::size_t& block)
    {
        if (receiverMode_ == ReceiverMode::ParallelWorker && expression->resolution == "field")
            return load(bindings.at(workerFieldParameter(expression->text)), expression->offset, block);
        const auto object = fieldReceiver(expression, bindings, block);
        return readReceiverField(object, fieldOffset(expression), type, expression->offset, block);
    }

    Operand selfReceiver(std::size_t offset, std::size_t block)
    {
        if (!hasReceiver_)
            throw Diagnostic("native lowering requires an explicit instance receiver here", offset);
        return load(0, offset, block);
    }

    Operand retainedSelf(std::size_t offset, std::size_t block)
    {
        retainsReceiver_ = true;
        return selfReceiver(offset, block);
    }

    // The hosted constructor projects its own fields onto model state.
    bool hostedConstructorState(const Expr& field) const
    {
        return hostedBindingConstructor_ &&
            (field->kind != "member" || field->children.front()->kind == "this");
    }

    Operand fieldReceiver(const Expr& field, const Bindings& bindings, std::size_t& block)
    {
        if (field->kind == "member" && !hostedConstructorState(field)) {
            const auto& target = field->children.front();
            budgetStep(block, target->offset);
            return receiver(target, bindings, block);
        }
        return selfReceiver(field->offset, block);
    }

    Operand receiver(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        if (receiverField(expression) && layouts_.sourceReceiver(expression->type)) {
            if (!function_.usesRuntime() && !function_.construction())
                throw Diagnostic("native owned receiver fields require hosted construction or execution", expression->offset);
            return readField(expression, ScalarType::Receiver, bindings, block);
        }
        if (expression->kind == "this" || expression->kind == "base")
            return selfReceiver(expression->offset, block);
        if (expression->resolution == "local") {
            const auto index = slot(expression, bindings);
            if (function_.slots.at(index).type == ScalarType::Receiver)
                return load(index, expression->offset, block);
        }
        if (!function_.usesRuntime() || !layouts_.sourceReceiver(expression->type))
            throw Diagnostic("native instance calls require a locally owned scalar object", expression->offset);
        auto object = unconvertedValue(expression, bindings, block);
        function_.blocks[block].instructions.push_back({expression->offset, RequireReceiver{object}});
        object.type = ScalarType::Receiver;
        return object;
    }

    Operand retainedReceiver(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        if (expression->resolution == "object_construct")
            return constructObject(expression, bindings, block, nullptr, true);
        if (receiverField(expression))
            return receiver(expression, bindings, block);
        if (expression->kind == "this" || expression->kind == "base") {
            if (function_.usesRuntime() || function_.construction())
                return retainedSelf(expression->offset, block);
        } else if (expression->resolution == "local") {
            const auto index = slot(expression, bindings);
            if (function_.slots.at(index).type == ScalarType::ClassReference)
                return receiver(expression, bindings, block);
            if (function_.construction() && index < function_.parameterCount)
                return receiver(expression, bindings, block);
            if (const auto found = localReceivers_.find(index); found != localReceivers_.end()) {
                const auto site = found->second;
                auto& allocation = std::get<AllocateObject>(function_.blocks[site.block].instructions[site.instruction].operation);
                if (function_.usesRuntime())
                    allocation.arena = true;
                if (allocation.arena)
                    return receiver(expression, bindings, block);
            }
        } else if (function_.usesRuntime() && layouts_.sourceReceiver(expression->type)) {
            return receiver(expression, bindings, block);
        }
        throw Diagnostic("native constructor cannot retain a stack receiver", expression->offset);
    }

    std::size_t fieldOffset(const Expr& expression)
    {
        const auto& layout = object_(expression->owner, expression->offset);
        const auto field = std::find_if(layout.fields.begin(), layout.fields.end(), [&](const auto& candidate) {
            return candidate.name == expression->text;
        });
        if (field == layout.fields.end())
            throw Diagnostic("native scalar object field was not resolved", expression->offset);
        return layouts_.receiverOffset(layout, static_cast<std::size_t>(field - layout.fields.begin()));
    }

    std::size_t slot(const Expr& expression, const Bindings& bindings) const
    {
        const auto found = bindings.find(expression->text);
        if (expression->kind != "name" || expression->resolution != "local" || found == bindings.end())
            throw Diagnostic("native scalar backend requires a local or parameter", expression->offset);

        return found->second;
    }

    Operand value(const Expr& expression, const Bindings& bindings, std::size_t& block, const Type* conversion = nullptr)
    {
        const bool awaited = function_.continuation() && expression->kind == "await";
        if (!awaited)
            budgetStep(block, expression->offset);
        const auto& target = conversion ? *conversion : expression->convertTo;
        if (target.name.empty() || target == expression->type)
            return awaited ? awaitExpression(expression, bindings, block) : unconvertedValue(expression, bindings, block);
        if ((function_.usesRuntime() || function_.construction()) && layouts_.receiverUpcast(expression->type, target)) {
            auto result = retainedReceiver(expression, bindings, block);
            result.type = layouts_.type(target, expression->offset);
            return result;
        }
        if ((function_.usesRuntime() || function_.construction()) && layouts_.sourceInterface(target)) {
            if (layouts_.sourceReceiver(expression->type)) {
                auto result = retainedReceiver(expression, bindings, block);
                result.type = ScalarType::Interface;
                return result;
            }
            if (layouts_.sourceInterface(expression->type))
                return unconvertedValue(expression, bindings, block);
        }
        if ((!function_.usesRuntime() && !function_.construction()) ||
            (target.name != "Optional" && target.name != "IResult"))
            throw Diagnostic("native scalar backend does not support implicit conversions", expression->offset);

        if (expression->type.name == "$null") {
            // Both arms can be null while the condition or a selected inner arm still has effects.
            if (expression->kind == "conditional")
                return conditionalValue(expression, bindings, block, &target);
            return optionalValue(layouts_.type(target, expression->offset), {}, block, expression->offset);
        }
        const auto initial = awaited ? awaitExpression(expression, bindings, block)
                                     : unconvertedValue(expression, bindings, block);
        return convertCheckedValue(initial, expression->type, target, block, expression->offset);
    }

    Operand convertCheckedValue(Operand initial, const Type& source, const Type& target, std::size_t block,
                                std::size_t offset)
    {
        if (source == target)
            return initial;

        if (source.name == "Result" && target.name == "IResult") {
            const auto type = layouts_.type(target, offset);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({offset, EraseResult{result, initial, type}});
            return {type, result};
        }

        if (target.name == "Optional") {
            const auto converted = convertCheckedValue(initial, source, target.arguments.front(), block, offset);
            return optionalValue(layouts_.type(target, offset), converted, block, offset);
        }
        if (layouts_.receiverUpcast(source, target)) {
            initial.type = layouts_.type(target, offset);
            return initial;
        }
        if ((layouts_.sourceReceiver(source) || layouts_.sourceInterface(source)) && layouts_.sourceInterface(target)) {
            initial.type = ScalarType::Interface;
            return initial;
        }
        throw Diagnostic("native scalar backend does not support this checked value conversion", offset);
    }

    Operand optionalValue(NativeType type, std::optional<Operand> value, std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ConstructOptional{result, type, value}});
        return {type, result};
    }

    static bool suspends(const Expr& expression)
    {
        return expression->kind == "await" || std::any_of(expression->children.begin(), expression->children.end(), suspends);
    }

    std::optional<std::size_t> retainOperand(Operand operand, std::size_t offset, std::size_t block)
    {
        if (!std::holds_alternative<Register>(operand.value))
            return std::nullopt;
        const auto slot = function_.slots.size();
        function_.slots.push_back({{}, operand.type, offset});
        function_.blocks[block].instructions.push_back({offset, Store{slot, operand, false}});
        return slot;
    }

    Operand evaluateAfter(Operand& earlier, const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        std::optional<std::size_t> slot;
        if (function_.continuation() && suspends(expression))
            slot = retainOperand(earlier, expression->offset, block);
        const auto later = value(expression, bindings, block);
        if (slot)
            earlier = load(*slot, expression->offset, block);
        return later;
    }

    std::vector<Operand> evaluateOperands(std::span<const Expr> expressions, const Bindings& bindings,
                                          std::size_t& block, std::optional<Operand> instance = {},
                                          bool arenaConstructor = false)
    {
        std::optional<std::size_t> lastSuspension;
        if (function_.continuation()) {
            for (std::size_t count = expressions.size(); count != 0; --count) {
                if (suspends(expressions[count - 1])) {
                    lastSuspension = count - 1;
                    break;
                }
            }
        }

        std::vector<Operand> operands;
        operands.reserve(expressions.size() + (instance.has_value() ? 1 : 0));
        struct RetainedOperand {
            std::size_t index;
            std::size_t slot;
            std::size_t offset;
        };
        std::vector<RetainedOperand> retained;
        const auto append = [&](Operand operand, std::size_t offset, bool acrossSuspension) {
            if (acrossSuspension) {
                if (const auto slot = retainOperand(operand, offset, block))
                    retained.push_back({operands.size(), *slot, offset});
            }
            operands.push_back(operand);
        };
        if (instance)
            append(*instance, expressions.empty() ? function_.offset : expressions.front()->offset, lastSuspension.has_value());
        for (std::size_t index = 0; index < expressions.size(); ++index) {
            const auto& expression = expressions[index];
            auto operand = [&]() {
                if (!arenaConstructor || !layouts_.sourceReceiver(expression->type) ||
                    (!expression->convertTo.name.empty() && expression->convertTo != expression->type))
                    return value(expression, bindings, block);
                budgetStep(block, expression->offset);
                return retainedReceiver(expression, bindings, block);
            }();
            append(operand, expression->offset, lastSuspension && index < *lastSuspension);
        }
        for (const auto& held : retained)
            operands[held.index] = load(held.slot, held.offset, block);
        return operands;
    }

    std::optional<Operand> concurrentDictionaryOperation(native_app_v19::ConcurrentDictionaryOperation operation,
                                                         Operand receiver, std::vector<Operand> arguments,
                                                         std::optional<NativeType> outputType, std::size_t block,
                                                         std::size_t offset)
    {
        std::size_t words = 0;
        for (const auto& argument : arguments)
            words += valueWords(argument.type, layouts_.view());
        std::optional<Operand> output;
        if (outputType) {
            output = Operand{*outputType, Register{registers_++}};
            words += valueWords(*outputType, layouts_.view());
        }
        function_.concurrentDictionaryWords = std::max(function_.concurrentDictionaryWords, words);
        function_.blocks[block].instructions.push_back(
            {offset, ConcurrentDictionaryCall{operation, receiver, std::move(arguments), output}});
        return output;
    }

    std::optional<Operand> callConcurrentDictionary(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::ConcurrentDictionaryOperation;
        const auto& receiverNode = call->children.front()->children.front();
        const auto receiver = value(receiverNode, bindings, block);
        auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, receiver);
        const auto evaluatedReceiver = arguments.front();
        arguments.erase(arguments.begin());

        static constexpr std::pair<std::string_view, Operation> operations[] = {
            {"concurrent_read_at", Operation::At},
            {"concurrent_read_tryGet", Operation::TryGet},
            {"concurrent_read_containsKey", Operation::Contains},
            {"concurrent_mutate_tryAdd", Operation::TryAdd},
            {"concurrent_mutate_set", Operation::Set},
            {"concurrent_mutate_tryRemove", Operation::TryRemove},
            {"concurrent_mutate_getOrAdd", Operation::GetOrAdd},
            {"concurrent_mutate_tryUpdate", Operation::TryUpdate},
            {"concurrent_mutate_clear", Operation::Clear}
        };
        const auto selected = std::find_if(std::begin(operations), std::end(operations), [&](const auto& candidate) {
            return candidate.first == call->resolution;
        });
        if (selected == std::end(operations))
            throw Diagnostic("unsupported native concurrent dictionary operation", call->offset);
        std::optional<NativeType> output;
        if (call->type != voidType)
            output = layouts_.type(call->type, call->offset);
        return concurrentDictionaryOperation(selected->second, evaluatedReceiver, std::move(arguments),
                                             output, block, call->offset);
    }

    static bool concurrentDictionaryCall(const Expr& expression)
    {
        return expression->kind == "call" && expression->resolution.starts_with("concurrent_") &&
            expression->resolution != "concurrent_construct" &&
            expression->children.front()->children.front()->type.name == "ConcurrentDictionary";
    }

    std::optional<Operand> concurrentSequenceOperation(native_app_v19::ConcurrentSequenceOperation operation,
                                                       Operand receiver, std::optional<Operand> input,
                                                       std::optional<NativeType> outputType, std::size_t block,
                                                       std::size_t offset)
    {
        std::optional<Operand> output;
        if (outputType)
            output = Operand{*outputType, Register{registers_++}};
        if (input)
            function_.concurrentSequenceWords = std::max(function_.concurrentSequenceWords,
                                                        valueWords(input->type, layouts_.view()));
        if (output)
            function_.concurrentSequenceWords = std::max(function_.concurrentSequenceWords,
                                                        valueWords(output->type, layouts_.view()));
        function_.blocks[block].instructions.push_back({offset, ConcurrentSequenceCall{operation, receiver, input, output}});
        return output;
    }

    std::optional<Operand> callConcurrentSequence(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::ConcurrentSequenceOperation;
        const auto& receiverNode = call->children.front()->children.front();
        const auto receiver = value(receiverNode, bindings, block);
        const auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, receiver);
        static constexpr std::pair<std::string_view, Operation> operations[] = {
            {"concurrent_mutate_enqueue", Operation::Append},
            {"concurrent_mutate_push", Operation::Append},
            {"concurrent_mutate_tryDequeue", Operation::TryTake},
            {"concurrent_mutate_tryPop", Operation::TryTake},
            {"concurrent_read_tryPeek", Operation::TryPeek},
            {"concurrent_mutate_clear", Operation::Clear},
            {"concurrent_read_items", Operation::Items}
        };
        const auto selected = std::find_if(std::begin(operations), std::end(operations), [&](const auto& candidate) {
            return candidate.first == call->resolution;
        });
        if (selected == std::end(operations))
            throw Diagnostic("unsupported native concurrent sequence operation", call->offset);
        std::optional<NativeType> result;
        if (call->type != voidType)
            result = layouts_.type(call->type, call->offset);
        const auto input = arguments.size() == 2 ? std::optional<Operand>{arguments[1]} : std::nullopt;
        return concurrentSequenceOperation(selected->second, arguments.front(), input, result, block, call->offset);
    }

    static bool concurrentSequenceCall(const Expr& expression)
    {
        return expression->kind == "call" && expression->resolution.starts_with("concurrent_") &&
            expression->resolution != "concurrent_construct" &&
            concurrentSequence(expression->children.front()->children.front()->type.name);
    }

    std::optional<Operand> waitSemaphore(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        const auto& receiver = call->children.front()->children.front();
        const auto semaphore = semaphoreOperand(receiver, bindings, block);
        const auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, semaphore);

        const bool timed = call->resolution == "sync_wait_timed";
        const auto timeout = timed ? arguments[1] : Operand{ScalarType::Integer, std::int64_t{-1}};
        const auto cancellationIndex = timed ? 2u : 1u;
        std::optional<Operand> cancellation;
        if (arguments.size() > cancellationIndex)
            cancellation = arguments[cancellationIndex];
        std::optional<Register> result;
        if (timed)
            result = Register{registers_++};

        function_.blocks[block].instructions.push_back(
            {call->offset, WaitSemaphore{result, arguments.front(), timeout, cancellation}});
        if (result)
            return Operand{ScalarType::Boolean, *result};
        return std::nullopt;
    }

    std::optional<Operand> atomicCall(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::AtomicOperation;
        const auto receiver = value(call->children.at(0)->children.at(0), bindings, block);
        const auto type = layouts_[receiver.type].fields.front().type;
        auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, receiver);
        const auto evaluatedReceiver = arguments.front();
        arguments.erase(arguments.begin());

        const auto& resolution = call->resolution;
        auto operation = Operation::Load;
        if (resolution == "atomic_store") {
            operation = Operation::Store;
        } else if (resolution == "atomic_exchange") {
            operation = Operation::Exchange;
        } else if (resolution == "atomic_compareExchange") {
            operation = Operation::CompareExchange;
        } else if (resolution == "atomic_add" || resolution == "atomic_increment" || resolution == "atomic_decrement") {
            operation = Operation::Add;
            if (resolution != "atomic_add")
                arguments.push_back({ScalarType::Integer, std::int64_t{resolution == "atomic_increment" ? 1 : -1}});
        } else if (resolution != "atomic_load") {
            throw Diagnostic("unsupported native Atomic operation", call->offset);
        }

        std::optional<Register> result;
        if (operation != Operation::Store)
            result = Register{registers_++};
        function_.blocks[block].instructions.push_back(
            {call->offset, AtomicCall{result, type, atomicKind(type), operation, evaluatedReceiver, std::move(arguments)}});
        if (result)
            return Operand{type, *result};
        return std::nullopt;
    }

    Operand unconvertedValue(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        if ((function_.usesRuntime() || function_.construction()) &&
            expression->resolution == "collection_construct" && expression->type.name == "FixedArray") {
            const auto initial = value(expression->children.at(1), bindings, block);
            return fillFixedArray(layouts_.type(expression->type, expression->offset), initial, expression->offset, block);
        }
        if (function_.usesRuntime() && layouts_.sourceReceiver(expression->type) &&
            (expression->resolution == "object_construct" || receiverField(expression) ||
             expression->kind == "this" ||
             (expression->resolution == "local" &&
              function_.slots.at(slot(expression, bindings)).type == ScalarType::Receiver))) {
            auto object = retainedReceiver(expression, bindings, block);
            object.type = layouts_.type(expression->type, expression->offset);
            return object;
        }
        if (function_.construction()) {
            if (expression->resolution == "object_construct")
                return constructObject(expression, bindings, block);
            if (receiverField(expression) && layouts_.sourceReceiver(expression->type))
                return receiver(expression, bindings, block);
        }
        if (expression->kind == "name" && expression->resolution == "local" &&
            function_.slots.at(slot(expression, bindings)).type == ScalarType::Receiver && !function_.construction())
            throw Diagnostic("native scalar objects can only be used as direct receivers", expression->offset);
        if (expression->kind == "name" && expression->resolution == "local") {
            const auto local = load(slot(expression, bindings), expression->offset, block);
            if (local.type != ScalarType::Semaphore || function_.mode == FunctionMode::Construction)
                return local;

            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, ShareSemaphore{result, local}});
            return {ScalarType::Semaphore, result};
        }

        if (expression->resolution == "context_construct") {
            if (!function_.usesRuntime() && !function_.construction())
                throw Diagnostic("native ContextOwner creation requires an invocation or construction", expression->offset);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, SynchronizationOperation{CreateContextOwner{result}}});
            return {ScalarType::ContextOwnerReference, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "sync_construct" &&
            expression->type == Type{"SemaphoreSlim", {}}) {
            const auto arguments = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
            std::optional<Operand> maximum;
            if (arguments.size() == 2)
                maximum = arguments[1];
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, CreateSharedSemaphore{result, arguments.front(), maximum}});
            return {ScalarType::Semaphore, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "sync_count") {
            const auto semaphore = semaphoreOperand(expression->children.front(), bindings, block);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, ReadSemaphoreCount{result, semaphore}});
            return {ScalarType::Integer, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "sync_release") {
            auto semaphore = semaphoreOperand(expression->children.front()->children.front(), bindings, block);
            auto count = Operand{ScalarType::Integer, std::int64_t{1}};
            if (expression->children.size() == 2)
                count = evaluateAfter(semaphore, expression->children[1], bindings, block);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, ReleaseSemaphore{result, semaphore, count}});
            return {ScalarType::Integer, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "concurrent_construct" &&
            expression->type.name == "ConcurrentDictionary") {
            const auto type = layouts_.type(expression->type, expression->offset);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({expression->offset, CreateConcurrentDictionary{result, type}});
            return Operand{type, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && concurrentDictionaryCall(expression))
            return callConcurrentDictionary(expression, bindings, block).value();
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "concurrent_construct" &&
            concurrentSequence(expression->type.name)) {
            const auto type = layouts_.type(expression->type, expression->offset);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({expression->offset, CreateConcurrentSequence{result, type}});
            return Operand{type, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && concurrentSequenceCall(expression))
            return callConcurrentSequence(expression, bindings, block).value();
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "sync_wait_timed")
            return waitSemaphore(expression, bindings, block).value();
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "sync_lock_construct") {
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({expression->offset, CreateLock{result}});
            return {ScalarType::LockReference, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution == "atomic_construct") {
            const auto initial = value(expression->children.at(1), bindings, block);
            const auto type = layouts_.type(expression->type, expression->offset);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, CreateAtomic{result, atomicKind(initial.type), initial}});
            return {type, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->resolution.starts_with("atomic_"))
            return atomicCall(expression, bindings, block).value();

        if (function_.usesRuntime() || function_.construction()) {
            if (expression->kind == "conditional")
                return conditionalValue(expression, bindings, block);
            if (expression->resolution.starts_with("string_"))
                return textCall(expression, bindings, block);
            if (expression->kind == "text") {
                try {
                    model::validate(expression->text);
                } catch (const model::ExecutionFailure& error) {
                    throw Diagnostic(error.what(), expression->offset);
                }
                const Register result{registers_++};
                function_.blocks[block].instructions.push_back({expression->offset, TextLiteral{result, expression->text}});
                return {ScalarType::Text, result};
            }
            if (expression->resolution == "construct")
                return constructRecord(expression, bindings, block);
            if (expression->resolution == "result_construct")
                return constructResult(expression, bindings, block);
            if (expression->kind == "named_construct")
                return constructNamedRecord(expression, bindings, block);
            if (expression->resolution == "record")
                return readRecordField(expression, bindings, block);
            if (expression->resolution == "pair_key" || expression->resolution == "pair_value") {
                const auto pair = value(expression->children.front(), bindings, block);
                return readRecordField(pair, expression->resolution == "pair_key" ? 0 : 1,
                                       expression->offset, block);
            }
            if (expression->resolution == "enum") {
                const auto type = layouts_.type(expression->type, expression->offset);
                const auto& members = layouts_[type].enumerators;
                const auto ordinal = std::find(members.begin(), members.end(), expression->text);
                if (ordinal == members.end())
                    throw std::logic_error("checked enum member is missing from its native layout");
                return {type, static_cast<std::int64_t>(ordinal - members.begin())};
            }
        }
        if (expression->resolution == "constant")
            return unconvertedValue(expression->constantValue, bindings, block);
        if (expression->resolution == "float_bits") {
            auto bits = value(expression->children.at(1), bindings, block);
            bits.type = layouts_.type(expression->type, expression->offset);
            return bits;
        }
        if ((function_.usesRuntime() || function_.construction()) &&
            expression->resolution == "floating_environment_observe") {
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({expression->offset,
                FloatingCall{result, native_app_v19::FloatingBuiltin::ObserveEnvironment,
                             ScalarType::Float, ScalarType::UInt32, {}}});
            return {ScalarType::UInt32, result};
        }
        if (isFloatingCall(expression))
            return floatingCall(expression, bindings, block);
        if ((function_.usesRuntime() || function_.construction()) && synchronousHostCall(expression))
            return hostCall(expression, bindings, block);
        if (function_.usesRuntime() || function_.construction()) {
            if (expression->resolution == "error_construct") {
                // The admitted text remains owned for the lifetime of the call or construction.
                auto message = value(expression->children.at(1), bindings, block);
                message.type = ScalarType::Error;
                return message;
            }
            if (expression->resolution == "error") {
                auto error = value(expression->children.at(0), bindings, block);
                error.type = ScalarType::Text;
                return error;
            }
            if (expression->resolution == "result")
                return readResult(expression, bindings, block);
        }
        if ((function_.usesRuntime() || function_.construction()) && expression->kind == "propagate")
            return propagateResult(expression, bindings, block).value();
        if (expression->resolution == "collection_construct" && expression->type == byteBufferType &&
            expression->children.front()->text == "Empty")
            return {ScalarType::ByteBuffer, ZeroValue{}};
        if ((function_.usesRuntime() || function_.construction()) &&
            expression->resolution == "collection_construct" && expression->type.name == "Array") {
            const auto operands = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
            const auto type = layouts_.type(expression->type, expression->offset);
            return arrayCall(native_app_v19::ArrayOperation::Create, type, {}, operands[0], operands[1],
                             type, expression->offset, block);
        }
        if (function_.usesRuntime() || function_.construction()) {
            if (expression->resolution == "list_construct" ||
                (expression->resolution == "collection_construct" &&
                 (expression->type.name == "HashSet" || expression->type.name == "Dictionary" ||
                  sequenceType(expression->type))))
                return {layouts_.type(expression->type, expression->offset), ZeroValue{}};
            if (expression->resolution == "optional") {
                const auto receiver = value(expression->children.at(0), bindings, block);
                const bool project = expression->text == "Value";
                const Register result{registers_++};
                function_.blocks[block].instructions.push_back({expression->offset, ReadOptional{result, receiver, project}});
                return {project ? layouts_[receiver.type].fields[1].type : NativeType{ScalarType::Boolean}, result};
            }
            if (hashSetCall(expression))
                return setCall(expression, bindings, block).value();
            if (sequenceCallExpression(expression))
                return sequenceCall(expression, bindings, block).value();
            if (dictionaryCallExpression(expression))
                return dictionaryCall(expression, bindings, block).value();
        }
        if (function_.mode == FunctionMode::Construction) {
            if (expression->resolution == "list_at")
                return readIndexedCollection(expression, bindings, block);
            if (expression->resolution == "collection_read_at") {
                const auto& collection = expression->children.at(0)->children.at(0)->type.name;
                if (collection == "Array" || collection == "FixedArray")
                    return readIndexedCollection(expression, bindings, block);
            }
        }

        if (function_.usesRuntime() || function_.construction()) {
            if (expression->kind == "member" && expression->resolution == "list") {
                if (expression->children.front()->type.name == "ConcurrentDictionary") {
                    const auto receiver = value(expression->children.front(), bindings, block);
                    return concurrentDictionaryOperation(native_app_v19::ConcurrentDictionaryOperation::Count,
                                                         receiver, {}, ScalarType::Integer, block, expression->offset).value();
                }
                if (concurrentSequence(expression->children.front()->type.name)) {
                    const auto receiver = value(expression->children.front(), bindings, block);
                    return concurrentSequenceOperation(native_app_v19::ConcurrentSequenceOperation::Count,
                                                       receiver, {}, ScalarType::Integer, block, expression->offset).value();
                }
                const auto receiver = collectionReceiver(expression->children.at(0), bindings, block);
                return collectionCall(CollectionOperation::Count, ScalarType::Integer, receiver, {}, expression->offset, block);
            }
            if (expression->resolution == "bytebuffer_length") {
                const auto buffer = value(expression->children.at(0), bindings, block);
                return byteLength(buffer, block, expression->offset);
            }
            if (byteBufferRead(expression))
                return readBytes(expression, bindings, block);
            if (expression->resolution == "unsigned_conversion" && expression->type == Type{"int", {}} &&
                expression->children.at(1)->type == Type{"uint32", {}}) {
                auto widened = value(expression->children.at(1), bindings, block);
                widened.type = ScalarType::Integer;
                return widened;
            }
            if (expression->resolution == "unsigned_wrapping") {
                const auto operands = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
                const auto operation = uint32Operation(expression->children.front()->text).value();
                return computeUInt32(operation, operands.at(0), operands.at(1), block, expression->offset);
            }
            if (expression->resolution == "unsigned_conversion") {
                const auto& argument = expression->children.at(1);
                if (expression->type == Type{"uint32", {}} && argument->type == Type{"int", {}}) {
                    const auto input = value(argument, bindings, block);
                    return computeUInt32(UInt32Operation::FromInt, input,
                                         {ScalarType::Integer, std::int64_t{0}}, block, expression->offset);
                }
            }
            if (expression->resolution == "pair_construct") {
                auto fields = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
                const auto type = layouts_.type(expression->type, expression->offset);
                const Register result{registers_++};
                function_.blocks[block].instructions.push_back(
                    {expression->offset, ConstructRecord{result, type, std::move(fields)}});
                return {type, result};
            }
            if (expression->resolution.starts_with("dictionary_"))
                return dictionaryProjection(expression, bindings, block);
            if (expression->resolution == "cancellation_none")
                return {ScalarType::CancellationToken, std::int64_t{0}};
            if (expression->resolution == "cancellation") {
                const auto token = value(expression->children.at(0), bindings, block);
                const Register result{registers_++};
                function_.blocks[block].instructions.push_back({expression->offset, CancellationRequested{result, token}});
                return {ScalarType::Boolean, result};
            }
        }
        if (function_.usesRuntime()) {
            if (expression->kind == "call" &&
                (expression->resolution == "list_at" || expression->resolution == "collection_read_at"))
                return readIndexedCollection(expression, bindings, block);
        }

        NativeType type = ScalarType::Service;
        if (!layouts_.service(expression->type)) {
            type = function_.usesRuntime() || function_.construction() ? layouts_.type(expression->type, expression->offset)
                                           : NativeType{scalarType(expression->type, expression->offset)};
        }
        if (receiverField(expression) || expression->resolution == "service")
            return readField(expression, type, bindings, block);
        if (expression->resolution == "source_interface_call" || expression->resolution == "class_virtual_call") {
            if (!function_.usesRuntime() && !function_.construction())
                throw Diagnostic("native dispatched calls require an invocation or construction", expression->offset);
            return callDispatched(expression, type, bindings, block).value();
        }
        if (directSynchronousCall(expression))
            return callDirect(expression, type, bindings, block).value();
        if (expression->kind == "integer")
            return {type, decimalLiteral(expression)};
        if (isFloating(type) && (expression->kind == "float" || expression->kind == "float32" ||
                                expression->resolution == "ieee_constant"))
            return floatingLiteral(expression, type);
        if (type == ScalarType::UInt32 && expression->kind == "uint32") {
            std::uint32_t literal;
            const auto& checked = expression->resolution;
            const auto parsed = std::from_chars(checked.data(), checked.data() + checked.size(), literal);
            if (parsed.ec != std::errc{} || parsed.ptr != checked.data() + checked.size())
                throw std::logic_error("checked uint32 expression is missing its normalized literal");
            return {type, static_cast<std::int64_t>(literal)};
        }
        if (expression->resolution == "minimum_int")
            return {ScalarType::Integer, std::numeric_limits<std::int64_t>::min()};
        if (function_.construction() && type == ScalarType::Integer && expression->kind == "unary" &&
            expression->text == "-" && expression->children.at(0)->kind == "integer") {
            const auto& operand = expression->children.at(0);
            budgetStep(block, operand->offset);
            return {type, -decimalLiteral(operand)};
        }
        if (expression->kind == "boolean")
            return {type, expression->text == "true"};
        if (expression->kind == "unary" && expression->text == "!") {
            const auto operand = value(expression->children.at(0), bindings, block);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {expression->offset, Compare{result, Predicate::Equal, operand, {ScalarType::Boolean, false}}});
            return {ScalarType::Boolean, result};
        }
        if ((function_.usesRuntime() || function_.construction()) && type == ScalarType::Integer &&
            expression->kind == "unary" && expression->text == "-") {
            const auto operand = value(expression->children.at(0), bindings, block);
            return checkedInteger(IntegerOperation::Subtract, {ScalarType::Integer, std::int64_t{0}},
                                  operand, block, expression->offset);
        }
        if (type == ScalarType::UInt32 && expression->kind == "unary" && expression->text == "~") {
            const auto operand = value(expression->children.at(0), bindings, block);
            return computeUInt32(UInt32Operation::Xor, operand,
                                 {type, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())},
                                 block, expression->offset);
        }
        if (isFloating(type) && expression->kind == "unary" && expression->text == "-") {
            const auto operand = value(expression->children.at(0), bindings, block);
            return computeFloating(FloatingArithmetic::Negate, operand, {}, block, expression->offset);
        }
        if (expression->kind == "name") {
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({expression->offset, Load{result, slot(expression, bindings)}});
            return {type, result};
        }
        if (expression->kind == "binary") {
            if (expression->text == "&&" || expression->text == "||")
                return shortCircuit(expression, bindings, block);
            auto left = value(expression->children.at(0), bindings, block);
            const auto right = evaluateAfter(left, expression->children.at(1), bindings, block);
            if ((function_.usesRuntime() || function_.construction()) &&
                type == ScalarType::Text && expression->text == "+")
                return emitTextCall(TextOperation::Concatenate, {left, right}, type, expression->offset, block);
            if ((function_.usesRuntime() || function_.construction()) && type == ScalarType::Integer) {
                if (const auto arithmetic = integerOperation(expression->text))
                    return checkedInteger(*arithmetic, left, right, block, expression->offset);
            }
            if (type == ScalarType::UInt32) {
                if (const auto arithmetic = uint32Operation(expression->text))
                    return computeUInt32(*arithmetic, left, right, block, expression->offset);
            }
            if (isFloating(type)) {
                if (const auto arithmetic = floatingArithmetic(expression->text))
                    return computeFloating(*arithmetic, left, right, block, expression->offset);
            }
            const auto operation = predicate(expression->text, expression->offset);
            const Register result{registers_++};
            const auto isObjectIdentity = [](NativeType value) {
                return value == ScalarType::ClassReference || value == ScalarType::Interface;
            };
            const bool comparesObjectIdentity = isObjectIdentity(left.type) && isObjectIdentity(right.type) &&
                (operation == Predicate::Equal || operation == Predicate::NotEqual);
            if (left.type == ScalarType::Integer || left.type == ScalarType::Boolean ||
                left.type == ScalarType::Enum || left.type == ScalarType::UInt32 || isFloating(left.type) ||
                comparesObjectIdentity) {
                function_.blocks[block].instructions.push_back({expression->offset, Compare{result, operation, left, right}});
            } else if ((function_.usesRuntime() || function_.construction()) && left.type == right.type &&
                       (operation == Predicate::Equal || operation == Predicate::NotEqual) &&
                       (usesAggregateLayout(left.type) || left.type == ScalarType::Text ||
                        left.type == ScalarType::Error || left.type == ScalarType::ByteBuffer)) {
                function_.blocks[block].instructions.push_back(
                    {expression->offset, CompareOwned{result, left, right, operation == Predicate::NotEqual}});
            } else {
                throw Diagnostic("native comparison does not support these operands", expression->offset);
            }
            return {type, result};
        }

        throw Diagnostic("unsupported native scalar expression: " + expression->kind, expression->offset);
    }

    Operand emitTextCall(TextOperation operation, std::vector<Operand> arguments, NativeType type,
                          std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, TextCall{result, operation, std::move(arguments), type}});
        return {type, result};
    }

    Operand textCall(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        using Entry = std::pair<std::string_view, TextOperation>;
        static constexpr std::array operations{
            Entry{"string_byte_length_is", TextOperation::ByteLengthIs},
            Entry{"string_byte_at_or", TextOperation::ByteAtOr},
            Entry{"string_starts_with", TextOperation::StartsWith},
            Entry{"string_from_int", TextOperation::FromInt},
            Entry{"string_utf8_length", TextOperation::Utf8Length},
            Entry{"string_utf8_slice", TextOperation::Utf8Slice},
            Entry{"string_utf8_index_of", TextOperation::Utf8IndexOf}
        };
        const auto found = std::find_if(operations.begin(), operations.end(), [&](const Entry& item) {
            return expression->resolution == item.first;
        });
        if (found == operations.end())
            throw Diagnostic("native lowering does not support this string operation", expression->offset);
        auto arguments = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
        const auto type = layouts_.type(expression->type, expression->offset);
        return emitTextCall(found->second, std::move(arguments), type, expression->offset, block);
    }

    void requireHostValue(NativeType type, std::size_t offset) const
    {
        switch (type.kind) {
        case ScalarType::Integer:
        case ScalarType::UInt32:
        case ScalarType::Float:
        case ScalarType::Float32:
        case ScalarType::ByteBuffer:
        case ScalarType::Boolean:
        case ScalarType::Text:
        case ScalarType::Error:
        case ScalarType::Enum:
            return;
        case ScalarType::Record:
        case ScalarType::List:
        case ScalarType::HashSet:
        case ScalarType::Queue:
        case ScalarType::Stack:
        case ScalarType::Dictionary:
        case ScalarType::Pair:
        case ScalarType::Optional:
        case ScalarType::Result:
        case ScalarType::FixedArray:
        case ScalarType::Array:
            for (const auto& field : layouts_[type].fields)
                requireHostValue(field.type, offset);
            return;
        default:
            throw Diagnostic("native synchronous host calls require owned values without receiver or service references",
                             offset);
        }
    }

    HostCall lowerHostCall(const Expr& expression, const Type& returned, const Bindings& bindings,
                           std::size_t& block)
    {
        const auto& target = expression->children[0];
        const auto& source = target->children[0];
        const auto service = value(source, bindings, block);
        auto arguments = evaluateOperands(std::span(expression->children).subspan(1), bindings, block, service);
        const auto bound = arguments.front();
        arguments.erase(arguments.begin());
        if (bound.type != ScalarType::Service || arguments.size() > native_app_v19::maximumArguments)
            throw Diagnostic("native host call requires a bound service and up to 32 arguments", expression->offset);

        const auto result = layouts_.type(returned, expression->offset);
        std::optional<NativeType> response = result;
        const bool wrapsHostStatus = result == ScalarType::Result && returned.arguments[1] == Type{"string", {}};
        if (wrapsHostStatus) {
            if (layouts_[result].fields.size() == 3)
                response = layouts_[result].fields[2].type;
            else
                response.reset();
        }
        if (response)
            requireHostValue(*response, expression->offset);

        auto signature = target->text + '(';
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index)
                signature += ',';
            const auto type = arguments[index].type;
            if (type == ScalarType::CancellationToken) {
                signature += "CancellationToken";
            } else {
                requireHostValue(type, expression->offset);
                signature += layouts_.typeName(type);
            }
        }
        signature += ')';
        return {std::nullopt, bound, layouts_.declaration(source->type)->sourceName, std::move(signature),
                std::move(arguments), result, response, wrapsHostStatus};
    }

    Operand hostCall(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        auto call = lowerHostCall(expression, expression->type, bindings, block);
        const Register output{registers_++};
        call.immediateResult = output;
        const auto type = call.resultType;
        function_.blocks[block].instructions.push_back({expression->offset, std::move(call)});
        if (binary32Checks_ != Binary32Checks::None && expression->resolution == "host_sync_value")
            requireBinary32Environment(block, expression->offset);
        return {type, output};
    }

    Operand awaitHostCall(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        auto call = lowerHostCall(expression->children.at(0), expression->type, bindings, block);
        const auto type = call.resultType;
        function_.blocks[block].instructions.push_back({expression->offset, std::move(call)});
        const auto resume = newBlock();
        terminate(block, expression->offset, AwaitHost{resume});
        block = resume;
        const Register output{registers_++};
        function_.childResultWords = std::max(function_.childResultWords, valueWords(type, layouts_.view()));
        function_.blocks[block].instructions.push_back({expression->offset, ResumedCall{output, type}});
        return {type, output};
    }

    Operand checkedInteger(IntegerOperation operation, Operand left, Operand right,
                           std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, CheckedInteger{result, operation, left, right}});
        return {ScalarType::Integer, result};
    }

    Operand computeUInt32(UInt32Operation operation, Operand left, Operand right,
                          std::size_t block, std::size_t offset)
    {
        if (!(function_.usesRuntime() || function_.construction()) && operation <= UInt32Operation::FromInt)
            throw Diagnostic("checked uint32 arithmetic requires an active native invocation", offset);
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, UInt32Compute{result, operation, left, right}});
        return {ScalarType::UInt32, result};
    }

    static bool isFloatingCall(const Expr& expression)
    {
        const auto& resolution = expression->resolution;
        if (resolution == "float_math" || resolution == "numeric_conversion" || resolution == "numeric_round")
            return true;
        if (resolution != "unsigned_conversion")
            return false;
        const auto& output = expression->type.name;
        const auto& input = expression->children.at(1)->type.name;
        return output == "float" || output == "float32" || input == "float" || input == "float32";
    }

    Operand floatingCall(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        using Op = native_app_v19::FloatingBuiltin;
        if (!(function_.usesRuntime() || function_.construction()))
            throw Diagnostic("floating math and conversions need an active native invocation", expression->offset);
        auto arguments = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
        const auto input = arguments.front().type;
        const auto output = layouts_.type(expression->type, expression->offset);
        const auto floatingType = isFloating(input) ? input : output;
        Op operation;
        if (expression->resolution == "float_math") {
            operation = floatingMath(expression->children.front()->text, arguments.size());
        } else if (expression->resolution == "numeric_round") {
            operation = Op::RoundToInt;
        } else if (output == ScalarType::Integer) {
            operation = Op::ToInt;
        } else if (output == ScalarType::UInt32) {
            operation = Op::ToUInt32;
        } else if (input == ScalarType::Integer) {
            operation = Op::FromInt;
        } else if (input == ScalarType::UInt32) {
            operation = Op::FromUInt32;
        } else {
            operation = output == ScalarType::Float ? Op::Widen : Op::Narrow;
        }
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back(
            {expression->offset, FloatingCall{result, operation, floatingType, output, std::move(arguments)}});
        return {output, result};
    }

    Operand computeFloating(FloatingArithmetic operation, Operand left, std::optional<Operand> right,
                            std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, FloatingCompute{result, operation, left, right}});
        return {left.type, result};
    }

    static bool byteBufferRead(const Expr& expression)
    {
        if (expression->kind != "call")
            return false;
        if (expression->resolution == "collection_construct")
            return expression->type == byteBufferType;
        if (!expression->resolution.starts_with("collection_read_"))
            return false;
        return expression->children.front()->children.front()->type == byteBufferType;
    }

    static bool byteBufferMutation(const Expr& expression)
    {
        return expression->kind == "call" && expression->resolution.starts_with("collection_mutate_") &&
            expression->children.front()->children.front()->type == byteBufferType;
    }

    Operand emitByteCall(native_app_v19::ByteOperation operation, NativeType resultType,
                         std::vector<Operand> arguments, std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back(
            {offset, ByteCall{result, operation, resultType, std::move(arguments)}});
        return {resultType, result};
    }

    Operand readBytes(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        using Op = native_app_v19::ByteOperation;
        auto operation = Op::FromUtf8;
        std::optional<Operand> receiver;
        if (expression->resolution != "collection_construct") {
            receiver = value(expression->children.front()->children.front(), bindings, block);
            if (expression->resolution == "collection_read_at")
                operation = Op::At;
            else if (expression->resolution == "collection_read_slice")
                operation = Op::Slice;
            else if (expression->resolution == "collection_read_indexOf")
                operation = Op::IndexOf;
            else if (expression->resolution == "collection_read_decodeUtf8")
                operation = Op::DecodeUtf8;
            else
                throw std::logic_error("unknown checked ByteBuffer read");
        }
        auto arguments = evaluateOperands(std::span(expression->children).subspan(1), bindings, block, receiver);
        return emitByteCall(operation, layouts_.type(expression->type, expression->offset),
                            std::move(arguments), block, expression->offset);
    }

    Operand byteLength(Operand buffer, std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ByteLength{result, buffer}});
        return {ScalarType::Integer, result};
    }

    void mutateBytes(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Op = native_app_v19::ByteOperation;
        const auto& target = call->children.front()->children.front();
        const auto operation = call->resolution == "collection_mutate_add" ? Op::Add : Op::Append;
        const auto& argument = call->children.at(1);
        const auto destination = recordTarget(target);
        const auto& root = destination.root;
        budgetLocation(target, root, block);
        if (receiverField(root)) {
            auto object = fieldReceiver(root, bindings, block);
            const auto input = evaluateAfter(object, argument, bindings, block);
            const auto field = receiverOffset(destination);
            const Register original{registers_++};
            function_.blocks[block].instructions.push_back(
                {call->offset, ReadField{original, object, field, ScalarType::ByteBuffer}});
            const auto updated = emitByteCall(operation, ScalarType::ByteBuffer,
                {{ScalarType::ByteBuffer, original}, input}, block, call->offset);
            function_.blocks[block].instructions.push_back({call->offset, WriteField{object, field, updated}});
            return;
        }
        const auto index = slot(root, bindings);
        const auto input = value(argument, bindings, block);
        const auto original = load(index, target->offset, block);
        auto buffer = original;
        for (const auto field : destination.path)
            buffer = readRecordField(buffer, field, target->offset, block);
        const auto updated = emitByteCall(operation, ScalarType::ByteBuffer, {buffer, input}, block, call->offset);
        const auto stored = destination.path.empty() ? updated :
            updateRecordField(original, destination.path, updated, block, target->offset);
        function_.blocks[block].instructions.push_back({call->offset, Store{index, stored, false}});
    }

    Operand shortCircuit(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const bool conjunction = expression->text == "&&";
        const auto left = value(expression->children.at(0), bindings, block);
        const auto resultSlot = function_.slots.size();
        function_.slots.push_back({{}, ScalarType::Boolean, expression->offset});
        function_.blocks[block].instructions.push_back(
            {expression->offset, Store{resultSlot, {ScalarType::Boolean, !conjunction}, false}});

        auto rightBlock = newBlock();
        const auto finished = newBlock();
        terminate(block, expression->offset, Branch{
            left, conjunction ? rightBlock : finished, conjunction ? finished : rightBlock});
        const auto right = value(expression->children.at(1), bindings, rightBlock);
        function_.blocks[rightBlock].instructions.push_back({expression->offset, Store{resultSlot, right, false}});
        terminate(rightBlock, expression->offset, Jump{finished});

        block = finished;
        return load(resultSlot, expression->offset, block);
    }

    Operand conditionalValue(const Expr& expression, const Bindings& bindings, std::size_t& block,
                             const Type* conversion = nullptr)
    {
        const auto condition = value(expression->children[0], bindings, block);
        const auto resultType = layouts_.type(conversion ? *conversion : expression->type, expression->offset);
        const auto slot = function_.slots.size();
        function_.slots.push_back({{}, resultType, expression->offset});
        const auto whenTrue = newBlock();
        const auto whenFalse = newBlock();
        const auto join = newBlock();
        terminate(block, expression->offset, Branch{condition, whenTrue, whenFalse});
        auto trueEnd = whenTrue;
        auto falseEnd = whenFalse;
        const auto primary = value(expression->children[1], bindings, trueEnd, conversion);
        const auto alternative = value(expression->children[2], bindings, falseEnd, conversion);
        if (function_.mode == FunctionMode::Construction &&
            (primary.type != resultType || alternative.type != resultType))
            throw Diagnostic("native constructor conditional branches require a common value layout", expression->offset);
        function_.blocks[trueEnd].instructions.push_back({expression->offset, Store{slot, primary, false}});
        function_.blocks[falseEnd].instructions.push_back({expression->offset, Store{slot, alternative, false}});
        terminate(trueEnd, expression->offset, Jump{join});
        terminate(falseEnd, expression->offset, Jump{join});
        block = join;
        return load(slot, expression->offset, block);
    }

    Operand constructResult(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto& name = expression->children.at(0)->text;
        auto kind = ResultKind::Success;
        if (name == "Failure")
            kind = ResultKind::Failure;
        else if (name == "Cancelled")
            kind = ResultKind::Cancelled;

        const auto offset = expression->offset;
        const auto type = layouts_.type(expression->type, offset);
        std::optional<Operand> argument;
        if (expression->children.size() == 2)
            argument = value(expression->children[1], bindings, block);
        const auto error = kind != ResultKind::Success && argument
            ? *argument : defaultValue(expression->type.arguments.at(1), offset, block);
        const auto payload = kind == ResultKind::Success ? argument : std::nullopt;
        return makeResult(type, kind, payload, error, offset, block);
    }

    Operand makeResult(NativeType type, ResultKind kind, std::optional<Operand> payload,
                       Operand error, std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ConstructResult{result, type, kind, payload, error}});
        return {type, result};
    }

    bool needsMaterializedDefault(const Type& source) const
    {
        if (source.name == "Result")
            return true;
        if (source.name == "FixedArray")
            return *source.fixedExtent != 0 && needsMaterializedDefault(source.arguments.front());
        if (source.name == "KeyValuePair")
            return std::ranges::any_of(source.arguments, [this](const auto& item) { return needsMaterializedDefault(item); });
        const auto* declaration = layouts_.declaration(source);
        if (!declaration || declaration->kind != "Record")
            return false;
        return std::ranges::any_of(declaration->fields, [this](const auto& field) {
            return field.initializer || needsMaterializedDefault(field.type);
        });
    }

    Operand defaultValue(const Type& source, std::size_t offset, std::size_t& block)
    {
        const auto type = layouts_.type(source, offset);
        if (type == ScalarType::FixedArray) {
            if (*source.fixedExtent == 0)
                return {type, ZeroValue{}};
            const auto initial = defaultValue(source.arguments.front(), offset, block);
            return fillFixedArray(type, initial, offset, block);
        }
        if (type == ScalarType::Pair) {
            std::vector<Operand> fields;
            for (const auto& field : source.arguments)
                fields.push_back(defaultValue(field, offset, block));
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {offset, ConstructRecord{result, type, std::move(fields)}});
            return {type, result};
        }
        if (type == ScalarType::Record) {
            if (!activeDefaults_.insert(type.layout).second)
                throw Diagnostic("native record default initialization is recursive", offset);
            std::vector<Operand> fields;
            for (const auto& field : layouts_.declaration(source)->fields) {
                if (field.initializer)
                    fields.push_back(value(field.initializer, {}, block));
                else
                    fields.push_back(defaultValue(field.type, offset, block));
            }
            activeDefaults_.erase(type.layout);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back({offset, ConstructRecord{result, type, std::move(fields)}});
            return {type, result};
        }
        if (type == ScalarType::Result) {
            const auto error = defaultValue(source.arguments.at(1), offset, block);
            return makeResult(type, ResultKind::Cancelled, {}, error, offset, block);
        }
        return {type, ZeroValue{}};
    }

    Operand readResult(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto receiver = value(expression->children.at(0), bindings, block);
        auto property = ResultProperty::IsSuccess;
        if (expression->text == "IsFailure")
            property = ResultProperty::IsFailure;
        else if (expression->text == "IsCancelled")
            property = ResultProperty::IsCancelled;
        else if (expression->text == "Value")
            property = ResultProperty::Value;
        else if (expression->text == "Error")
            property = ResultProperty::Error;
        return readResult(receiver, property, expression->offset, block);
    }

    Operand readResult(Operand receiver, ResultProperty property, std::size_t offset, std::size_t block)
    {
        NativeType type = ScalarType::Boolean;
        if (property == ResultProperty::Value || property == ResultProperty::SuccessPayload)
            type = layouts_[receiver.type].fields.at(2).type;
        else if (property == ResultProperty::Error)
            type = layouts_[receiver.type].fields.at(receiver.type == ScalarType::ResultInterface ? 4 : 1).type;
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ReadResult{result, receiver, property}});
        return {type, result};
    }

    std::optional<Operand> propagateResult(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto offset = expression->offset;
        const auto result = value(expression->children.at(0), bindings, block);
        const auto success = readResult(result, ResultProperty::IsSuccess, offset, block);
        const auto continued = newBlock();
        const auto unsuccessful = newBlock();
        terminate(block, offset, Branch{success, continued, unsuccessful});

        const auto cancelled = readResult(result, ResultProperty::IsCancelled, offset, unsuccessful);
        const auto error = readResult(result, ResultProperty::Error, offset, unsuccessful);
        const auto cancellation = newBlock();
        const auto failure = newBlock();
        terminate(unsuccessful, offset, Branch{cancelled, cancellation, failure});
        returnValue(cancellation, makeResult(function_.result, ResultKind::Cancelled, {}, error, offset, cancellation), offset);
        returnValue(failure, makeResult(function_.result, ResultKind::Failure, {}, error, offset, failure), offset);

        block = continued;
        if (expression->type == Type{"void", {}})
            return std::nullopt;
        return readResult(result, ResultProperty::SuccessPayload, offset, continued);
    }

    Operand constructRecord(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto type = layouts_.type(expression->type, expression->offset);
        if (type != ScalarType::Record)
            throw std::logic_error("checked record constructor has no native record layout");
        auto fields = evaluateOperands(std::span(expression->children).subspan(1), bindings, block);
        const auto& layout = layouts_[type];
        if (fields.size() != layout.fields.size() ||
            !std::equal(fields.begin(), fields.end(), layout.fields.begin(), [](const Operand& value, const ValueField& field) {
                return value.type == field.type;
            }))
            throw std::logic_error("checked record arguments do not match the native layout");
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back(
            {expression->offset, ConstructRecord{result, type, std::move(fields)}});
        return {type, result};
    }

    std::size_t recordFieldIndex(NativeType record, std::string_view name) const
    {
        const auto& fields = layouts_[record].fields;
        const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& member) {
            return member.name == name;
        });
        if (field == fields.end())
            throw std::logic_error("checked record field is missing from its native layout");
        return static_cast<std::size_t>(field - fields.begin());
    }

    Operand readRecordField(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto record = value(expression->children.at(0), bindings, block);
        const auto index = recordFieldIndex(record.type, expression->text);
        return readRecordField(record, index, expression->offset, block);
    }

    Operand readRecordField(Operand record, std::size_t index, std::size_t offset, std::size_t block)
    {
        const auto type = layouts_[record.type].fields[index].type;
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ReadRecordField{result, record, index, type}});
        return {type, result};
    }

    Operand updateRecordField(Operand record, std::vector<std::size_t> path, Operand value,
                              std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back(
            {offset, UpdateRecordField{result, record, std::move(path), value}});
        return {record.type, result};
    }

    Operand constructNamedRecord(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        auto record = defaultValue(expression->type, expression->offset, block);
        for (const auto& field : expression->children) {
            const auto index = recordFieldIndex(record.type, field->text);
            const auto input = evaluateAfter(record, field->children.front(), bindings, block);
            record = updateRecordField(record, {index}, input, block, field->offset);
        }
        return record;
    }

    struct RecordTarget {
        Expr root;
        std::vector<std::size_t> path;
    };

    RecordTarget recordTarget(Expr root)
    {
        std::vector<std::size_t> path;
        while (root->resolution == "record") {
            auto parent = root->children.front();
            const auto type = layouts_.type(parent->type, parent->offset);
            path.push_back(recordFieldIndex(type, root->text));
            root = std::move(parent);
        }
        std::reverse(path.begin(), path.end());
        return {std::move(root), std::move(path)};
    }

    std::size_t receiverOffset(const RecordTarget& target)
    {
        auto offset = fieldOffset(target.root);
        if (target.path.empty())
            return offset;
        auto type = layouts_.type(target.root->type, target.root->offset);
        for (const auto index : target.path) {
            const auto fields = std::span(layouts_[type].fields);
            for (const auto& field : fields.first(index))
                offset += valueWords(field.type, layouts_.view());
            type = fields[index].type;
        }
        return offset;
    }

    void budgetLocation(const Expr& target, const Expr& root, std::size_t block)
    {
        // Ordinary object field writes charge their evaluated object handle.
        // Local/model storage also charges the traced member path itself.
        if (root->resolution == "object_field" && !hostedConstructorState(root))
            return;
        for (auto part = target; part != root; part = part->children.front())
            budgetStep(block, part->offset);
        budgetStep(block, root->offset);
    }

    void assignRecordField(const Statement& statement, const Bindings& bindings, std::size_t& block)
    {
        auto target = recordTarget(statement.target);
        const auto& root = target.root;
        budgetLocation(statement.target, root, block);
        if (receiverField(root)) {
            auto object = fieldReceiver(root, bindings, block);
            const auto input = evaluateAfter(object, statement.expression, bindings, block);
            function_.blocks[block].instructions.push_back(
                {statement.offset, WriteField{object, receiverOffset(target), input}});
            return;
        }
        const auto local = slot(root, bindings);
        const auto input = value(statement.expression, bindings, block);
        const auto record = load(local, statement.target->offset, block);
        const auto updated = updateRecordField(record, std::move(target.path), input, block, statement.offset);
        function_.blocks[block].instructions.push_back({statement.offset, Store{local, updated, false}});
    }

    Operand collectionReceiver(const Expr& expression, const Bindings& bindings, std::size_t& block,
                              bool takeSnapshot = false)
    {
        const auto receiver = value(expression, bindings, block);
        if (takeSnapshot && receiver.type == ScalarType::ConcurrentDictionaryReference) {
            const auto list = layouts_.type(Type{"List", {{"KeyValuePair", expression->type.arguments}}}, expression->offset);
            return concurrentDictionaryOperation(native_app_v19::ConcurrentDictionaryOperation::Entries,
                                                 receiver, {}, list, block, expression->offset).value();
        }
        if (takeSnapshot && receiver.type == ScalarType::ConcurrentSequenceReference) {
            const auto list = layouts_.type(Type{"List", expression->type.arguments}, expression->offset);
            return concurrentSequenceOperation(native_app_v19::ConcurrentSequenceOperation::Items,
                                               receiver, {}, list, block, expression->offset).value();
        }
        if (!usesCollectionHandle(receiver.type.kind) && receiver.type != ScalarType::ByteBuffer &&
            receiver.type != ScalarType::FixedArray)
            throw Diagnostic("native collection operation requires a supported collection", expression->offset);
        return receiver;
    }

    Operand setItems(Operand set, NativeType resultType, std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back(
            {offset, SetCall{SetOperation::ToList, result, set, {}, {}, resultType}});
        return {resultType, result};
    }

    std::optional<Operand> setCall(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        const auto& target = call->children.at(0)->children.at(0);
        if (call->resolution == "collection_read_items")
            return setItems(value(target, bindings, block), layouts_.type(call->type, call->offset), call->offset, block);

        std::optional<Operand> set;
        std::optional<Operand> key;
        std::optional<Operand> object;
        std::optional<CollectionUpdate> local;
        std::size_t field = 0;
        const bool clears = call->resolution == "collection_mutate_clear";
        const bool mutates = call->resolution.starts_with("collection_mutate_");
        if (mutates) {
            auto place = collectionPlace(target, bindings, block);
            if (place.object) {
                object = place.object;
                field = receiverOffset(place.target);
                if (!clears)
                    key = evaluateAfter(*object, call->children.at(1), bindings, block);
                const auto type = layouts_.type(target->type, target->offset);
                const Register original{registers_++};
                if (!clears)
                    function_.blocks[block].instructions.push_back({target->offset, ReadField{original, *object, field, type}});
                set = Operand{type, original};
            } else {
                if (!clears)
                    key = value(call->children.at(1), bindings, block);
                local = readCollection(target, place, bindings, block);
                set = local->collection;
            }
        } else {
            set = value(target, bindings, block);
            key = evaluateAfter(*set, call->children.at(1), bindings, block);
        }

        const auto store = [&](Operand updated) {
            if (object)
                function_.blocks[block].instructions.push_back({call->offset, WriteField{*object, field, updated}});
            else
                writeCollection(*local, updated, call->offset, block);
        };
        if (clears) {
            store({set->type, ZeroValue{}});
            return std::nullopt;
        }
        auto operation = SetOperation::Contains;
        if (call->resolution == "collection_mutate_add")
            operation = SetOperation::Add;
        else if (call->resolution == "collection_mutate_remove")
            operation = SetOperation::Remove;
        else if (call->resolution != "collection_read_contains")
            throw Diagnostic("unsupported native HashSet method", call->offset);
        const Register result{registers_++};
        std::optional<Register> updated;
        if (mutates)
            updated = Register{registers_++};
        function_.blocks[block].instructions.push_back(
            {call->offset, SetCall{operation, result, *set, key, updated, ScalarType::Boolean}});
        if (updated)
            store({set->type, *updated});
        return Operand{ScalarType::Boolean, result};
    }

    Operand fillFixedArray(NativeType type, Operand initial, std::size_t offset, std::size_t block)
    {
        if (layouts_[type].fixedExtent == 0)
            return {type, ZeroValue{}};
        const bool validateOwnership = !fixedArrayHasStaticOwnership(type, layouts_.view());
        if (validateOwnership && !(function_.usesRuntime() || function_.construction()))
            throw Diagnostic("native fixed array construction needs runtime ownership validation", offset);
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, FillFixedArray{result, type, initial, validateOwnership}});
        return {type, result};
    }

    Operand fixedArrayAccess(Operand array, Operand index, NativeType element, std::optional<Operand> replacement,
                             std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        const bool validateOwnership = replacement && !fixedArrayHasStaticOwnership(array.type, layouts_.view());
        auto& instructions = function_.blocks[block].instructions;
        const auto* literal = std::get_if<std::int64_t>(&index.value);
        if (literal && static_cast<std::uint64_t>(*literal) < layouts_[array.type].fixedExtent &&
            !validateOwnership) {
            const auto position = static_cast<std::size_t>(*literal);
            if (replacement)
                instructions.push_back({offset, UpdateRecordField{result, array, {position}, *replacement}});
            else
                instructions.push_back({offset, ReadRecordField{result, array, position, element}});
        } else {
            instructions.push_back({offset, FixedArrayIndex{result, array, index, element, replacement, validateOwnership}});
        }
        return {replacement ? array.type : element, result};
    }

    Operand arrayCall(native_app_v19::ArrayOperation operation, NativeType array, std::optional<Operand> receiver,
                      Operand index, std::optional<Operand> item, NativeType resultType,
                      std::size_t offset, std::size_t block)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset, ArrayCall{operation, result, array, receiver, index, item}});
        return {resultType, result};
    }

    Operand readIndexedCollection(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        auto receiver = collectionReceiver(call->children.at(0)->children.at(0), bindings, block);
        const auto index = evaluateAfter(receiver, call->children.at(1), bindings, block);
        const auto element = layouts_.type(call->type, call->offset);
        return collectionCall(CollectionOperation::At, element, receiver, index, call->offset, block);
    }

    Operand collectionCall(CollectionOperation operation, NativeType resultType, Operand receiver,
                           std::optional<Operand> index, std::size_t offset, std::size_t block)
    {
        const auto collection = receiver.type;
        if (collection == ScalarType::FixedArray) {
            if (operation == CollectionOperation::Count)
                return {ScalarType::Integer, static_cast<std::int64_t>(layouts_[collection].fixedExtent)};
            return fixedArrayAccess(receiver, *index, resultType, {}, offset, block);
        }
        if (collection == ScalarType::Array && operation == CollectionOperation::At)
            return arrayCall(native_app_v19::ArrayOperation::At, collection, receiver, *index, {}, resultType, offset, block);
        const Register result{registers_++};
        if (isSequence(collection.kind) && operation == CollectionOperation::At) {
            const Operand item{resultType, result};
            function_.blocks[block].instructions.push_back(
                {offset, SequenceCall{native_app_v19::SequenceOperation::At, receiver, {}, index, {}, item}});
            return item;
        }
        if (collection == ScalarType::Dictionary && operation == CollectionOperation::At) {
            const Operand pair{resultType, result};
            function_.blocks[block].instructions.push_back(
                {offset, DictionaryCall{native_app_v19::DictionaryOperation::Iterate, receiver,
                                        {}, {}, {}, index, {}, pair}});
            return pair;
        }
        function_.blocks[block].instructions.push_back(
            {offset, CollectionCall{operation, result, receiver, index}});
        return {resultType, result};
    }

    struct CollectionPlace {
        RecordTarget target;
        std::optional<Operand> object;
    };

    CollectionPlace collectionPlace(const Expr& target, const Bindings& bindings, std::size_t& block)
    {
        auto place = recordTarget(target);
        budgetLocation(target, place.root, block);
        std::optional<Operand> object;
        if (receiverField(place.root))
            object = fieldReceiver(place.root, bindings, block);
        return {std::move(place), object};
    }

    struct CollectionUpdate {
        std::variant<std::size_t, Operand> destination;
        std::size_t offset;
        std::vector<std::size_t> path;
        Operand original;
        Operand collection;
    };

    CollectionUpdate readCollection(const Expr& target, CollectionPlace& place, const Bindings& bindings,
                                    std::size_t block)
    {
        if (place.object) {
            const auto offset = receiverOffset(place.target);
            const Register result{registers_++};
            const Operand collection{layouts_.type(target->type, target->offset), result};
            function_.blocks[block].instructions.push_back(
                {target->offset, ReadField{result, *place.object, offset, collection.type}});
            return {*place.object, offset, {}, collection, collection};
        }
        const auto destination = slot(place.target.root, bindings);
        const auto original = load(destination, target->offset, block);
        auto collection = original;
        for (const auto field : place.target.path)
            collection = readRecordField(collection, field, target->offset, block);
        return {destination, 0, std::move(place.target.path), original, collection};
    }

    void writeCollection(CollectionUpdate& update, Operand value, std::size_t offset, std::size_t block)
    {
        auto& instructions = function_.blocks[block].instructions;
        if (const auto* receiver = std::get_if<Operand>(&update.destination)) {
            instructions.push_back({offset, WriteField{*receiver, update.offset, value}});
        } else {
            if (!update.path.empty())
                value = updateRecordField(update.original, std::move(update.path), value, block, offset);
            instructions.push_back({offset, Store{std::get<std::size_t>(update.destination), value, false}});
        }
    }

    std::optional<Operand> sequenceCall(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::SequenceOperation;
        const auto& operation = call->resolution;
        const auto& target = call->children.front()->children.front();
        const bool mutating = operation.starts_with("collection_mutate_");
        const bool clearing = operation == "collection_mutate_clear";
        const bool takes = operation == "collection_mutate_dequeue" || operation == "collection_mutate_pop";
        const bool tries = operation == "collection_mutate_tryDequeue" || operation == "collection_mutate_tryPop";
        const bool appends = operation == "collection_mutate_enqueue" || operation == "collection_mutate_push";
        if (!clearing && !takes && !tries && !appends && operation != "collection_read_peek")
            throw Diagnostic("unsupported native sequence operation", call->offset);

        std::optional<Operand> input;
        std::optional<CollectionUpdate> destination;
        std::optional<Operand> sequence;
        if (mutating) {
            auto place = collectionPlace(target, bindings, block);
            if (appends) {
                const auto& item = call->children.at(1);
                input = place.object ? evaluateAfter(*place.object, item, bindings, block) : value(item, bindings, block);
            }
            destination = readCollection(target, place, bindings, block);
            sequence = destination->collection;
        } else {
            sequence = value(target, bindings, block);
        }

        std::optional<Operand> result;
        if (call->type != voidType)
            result = Operand{layouts_.type(call->type, call->offset), Register{registers_++}};
        if (clearing) {
            sequence = Operand{sequence->type, ZeroValue{}};
        } else {
            auto selected = Operation::Peek;
            if (appends)
                selected = Operation::Append;
            else if (takes)
                selected = Operation::Take;
            else if (tries)
                selected = Operation::TryTake;
            std::optional<Register> updated;
            if (mutating)
                updated = Register{registers_++};
            function_.blocks[block].instructions.push_back(
                {call->offset, SequenceCall{selected, *sequence, input, {}, updated, result}});
            if (updated)
                sequence = Operand{sequence->type, *updated};
        }
        if (destination)
            writeCollection(*destination, *sequence, call->offset, block);
        return result;
    }

    Operand dictionaryProjection(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::DictionaryOperation;
        auto operation = Operation::Entries;
        if (expression->resolution == "dictionary_keys")
            operation = Operation::Keys;
        else if (expression->resolution == "dictionary_values")
            operation = Operation::Values;
        const auto receiver = value(expression->children.front(), bindings, block);
        if (receiver.type == ScalarType::ConcurrentDictionaryReference) {
            using ConcurrentOperation = native_app_v19::ConcurrentDictionaryOperation;
            auto concurrent = ConcurrentOperation::Entries;
            if (operation == Operation::Keys)
                concurrent = ConcurrentOperation::Keys;
            else if (operation == Operation::Values)
                concurrent = ConcurrentOperation::Values;
            const auto type = layouts_.type(expression->type, expression->offset);
            return concurrentDictionaryOperation(concurrent, receiver, {}, type, block, expression->offset).value();
        }
        if (receiver.type != ScalarType::Dictionary)
            throw Diagnostic("native dictionary projection requires an invocation-owned Dictionary", expression->offset);
        const auto type = layouts_.type(expression->type, expression->offset);
        const Operand projected{type, Register{registers_++}};
        function_.blocks[block].instructions.push_back(
            {expression->offset, DictionaryCall{operation, receiver, {}, {}, layouts_[type].fields[0].type,
                                                {}, {}, projected}});
        return projected;
    }

    std::optional<Operand> dictionaryCall(const Expr& call, const Bindings& bindings, std::size_t& block)
    {
        using Operation = native_app_v19::DictionaryOperation;
        const auto& target = call->children.front()->children.front();
        const auto& selected = call->resolution;
        const bool mutates = selected.starts_with("collection_mutate_");
        std::optional<CollectionPlace> place;
        if (mutates)
            place = collectionPlace(target, bindings, block);
        if (selected == "collection_mutate_clear") {
            auto destination = readCollection(target, *place, bindings, block);
            writeCollection(destination, {destination.collection.type, ZeroValue{}}, call->offset, block);
            return std::nullopt;
        }
        static constexpr std::pair<std::string_view, Operation> operations[] = {
            {"collection_read_at", Operation::At},
            {"collection_read_tryGet", Operation::TryGet},
            {"collection_read_containsKey", Operation::Contains},
            {"collection_mutate_add", Operation::Add},
            {"collection_mutate_set", Operation::Set},
            {"collection_mutate_remove", Operation::Remove}
        };
        const auto operation = std::find_if(std::begin(operations), std::end(operations), [&](const auto& entry) {
            return entry.first == selected;
        });
        if (operation == std::end(operations))
            throw Diagnostic("unsupported native Dictionary operation", call->offset);

        const auto arguments = std::span(call->children).subspan(1);
        const auto object = place ? place->object : value(target, bindings, block);
        const auto values = evaluateOperands(arguments, bindings, block, object);
        std::optional<CollectionUpdate> destination;
        if (place) {
            if (object)
                place->object = values.front();
            destination = readCollection(target, *place, bindings, block);
        }
        const auto dictionary = destination ? destination->collection : values.front();
        const auto first = object ? 1U : 0U;
        std::optional<Operand> mapped;
        if (values.size() > first + 1)
            mapped = values[first + 1];
        std::optional<Operand> result;
        if (call->type != voidType)
            result = Operand{layouts_.type(call->type, call->offset), Register{registers_++}};
        std::optional<Register> updated;
        if (destination)
            updated = Register{registers_++};
        function_.blocks[block].instructions.push_back(
            {call->offset, DictionaryCall{operation->second, dictionary, values[first], mapped, {}, {}, updated, result}});
        if (destination)
            writeCollection(*destination, {dictionary.type, *updated}, call->offset, block);
        return result;
    }

    Operand constructObject(const Expr& constructor, const Bindings& bindings, std::size_t& block,
                            std::optional<ReceiverAllocation>* localAllocation = nullptr, bool requireArena = false)
    {
        const auto& layout = object_(constructor->owner, constructor->offset);
        const auto tag = layouts_.receiverTag(layout);
        if (tag && !function_.construction() && !function_.usesRuntime())
            throw Diagnostic("native dispatched objects require an invocation", constructor->offset);
        const bool contextual = function_.mode == FunctionMode::Contextual;
        const bool usesConstructorAbi = function_.construction() || function_.usesRuntime();
        const auto words = layouts_.receiverOffset(layout, layout.fields.size());
        if (requireArena && !usesConstructorAbi)
            throw Diagnostic("native constructor cannot retain a stack receiver", constructor->offset);
        // Fieldless contextual locals can use frame storage until a retaining
        // constructor requires a lifetime beyond the synchronous frame.
        const bool arena = usesConstructorAbi &&
            (requireArena || function_.construction() || tag != 0 || (contextual && words != 0));
        auto arguments = evaluateOperands(std::span(constructor->children).subspan(1), bindings, block, {},
                                          usesConstructorAbi);
        const auto destination = resolve_(constructor);
        budgetStep(block, constructor->offset, destination.unmeteredWork);
        const Register result{registers_++};
        const Operand object{ScalarType::Receiver, result};
        if (localAllocation)
            *localAllocation = ReceiverAllocation{block, function_.blocks[block].instructions.size()};
        function_.blocks[block].instructions.push_back(
            {constructor->offset, AllocateObject{result, 0, words, tag, arena,
                                                  usesConstructorAbi}});
        arguments.insert(arguments.begin(), object);
        function_.blocks[block].instructions.push_back(
            {constructor->offset, DirectCall{{}, destination.symbol, std::move(arguments),
                                            usesConstructorAbi ? CallAbi::Construction : CallAbi::Scalar, {},
                                            usesConstructorAbi}});
        return object;
    }

    Operand initializeLocal(const Statement& statement, const Bindings& bindings, std::size_t& block,
                            std::optional<ReceiverAllocation>& receiverAllocation)
    {
        if (function_.continuation() && statement.type.name == "Atomic" &&
            statement.expression->resolution == "atomic_construct" && !escapingAtomics_.contains(statement.name)) {
            const auto& create = statement.expression;
            budgetStep(block, create->offset);
            const auto initial = value(create->children.at(1), bindings, block);
            const Register result{registers_++};
            function_.blocks[block].instructions.push_back(
                {create->offset, CreateAtomic{result, atomicKind(initial.type), initial, localAtomicSites_++}});
            return {layouts_.type(statement.type, statement.offset), result};
        }
        if (function_.continuation() && statement.type == Type{"SemaphoreSlim", {}} &&
            statement.expression->resolution == "sync_construct") {
            if (semaphoreSites_ == native_app_v19::maximumSemaphoreSites)
                throw Diagnostic("native semaphore storage exceeds the per-function limit", statement.offset);

            const auto& create = statement.expression;
            budgetStep(block, create->offset);
            const auto arguments = evaluateOperands(std::span(create->children).subspan(1), bindings, block);
            std::optional<Operand> maximum;
            if (arguments.size() == 2)
                maximum = arguments[1];
            const auto site = semaphoreSites_++;
            function_.blocks[block].instructions.push_back(
                {create->offset, CreateSemaphore{site, arguments.front(), maximum}});
            return {ScalarType::Semaphore, static_cast<std::int64_t>(site)};
        }
        if (statement.expression && statement.expression->resolution == "object_construct") {
            const auto& constructor = statement.expression;
            if (constructor->type != statement.type)
                return value(constructor, bindings, block);
            budgetStep(block, constructor->offset);
            return constructObject(constructor, bindings, block, &receiverAllocation);
        }
        return value(statement.expression, bindings, block);
    }

    std::size_t newBlock()
    {
        const auto index = function_.blocks.size();
        function_.blocks.emplace_back();
        function_.blocks.back().faultTarget = faultTarget_;
        return index;
    }

    Operand semaphoreOperand(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        if (expression->resolution == "local") {
            budgetStep(block, expression->offset);
            return load(slot(expression, bindings), expression->offset, block);
        }
        return value(expression, bindings, block);
    }

    Operand awaitSemaphore(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto& call = expression->children.at(0);
        const bool timed = call->resolution == "sync_wait_async_timed";
        if (!timed && call->resolution != "sync_wait_async")
            throw Diagnostic("native suspension requires a local semaphore wait or a closed source call",
                             expression->offset);

        const auto& receiver = call->children.at(0)->children.at(0);
        const auto semaphore = semaphoreOperand(receiver, bindings, block);
        if (semaphore.type != ScalarType::Semaphore)
            throw Diagnostic("native suspension requires an invocation-owned semaphore", expression->offset);
        const auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, semaphore);
        auto timeout = Operand{ScalarType::Integer, std::int64_t{-1}};
        if (timed)
            timeout = arguments[1];
        std::optional<Operand> cancellation;
        const auto tokenIndex = timed ? 2U : 1U;
        if (arguments.size() > tokenIndex)
            cancellation = arguments[tokenIndex];

        const auto resume = newBlock();
        terminate(block, expression->offset, Suspend{arguments[0], timeout, cancellation, resume});
        block = resume;
        if (!timed)
            return {ScalarType::Boolean, false};
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({expression->offset, ResumedWait{result}});
        return {ScalarType::Boolean, result};
    }

    template<class EmitTarget>
    void dispatchReceiver(const Expr& call, Operand object, std::size_t block, const EmitTarget& emit)
    {
        const Register tag{registers_++};
        function_.blocks[block].instructions.push_back({call->offset, ReadReceiverTag{tag, object}});
        std::vector<std::pair<std::size_t, std::size_t>> branches;
        for (const auto& [type, symbol] : dispatch_(call)) {
            const auto target = newBlock();
            branches.emplace_back(type, target);
            emit(target, symbol);
        }
        const auto fallback = newBlock();
        terminate(fallback, call->offset, InvalidDispatch{});
        terminate(block, call->offset, DispatchReceiver{{ScalarType::Integer, tag}, std::move(branches), fallback});
    }

    std::optional<Operand> callDirect(const Expr& call, std::optional<NativeType> type,
                                      const Bindings& bindings, std::size_t& block)
    {
        const auto destination = resolve_(call);
        std::optional<Operand> object;
        if (receiverMode_ == ReceiverMode::ParallelWorker && receiverFreeWorkerCall(call)) {
            // A checked object helper still charges for its implicit receiver.
            if (call->statelessWorkerCall)
                budgetStep(block, call->offset);
        } else if (call->resolution == "object_call") {
            const auto& target = call->children.at(0)->children.at(0);
            budgetStep(block, target->offset);
            object = destination.retainsReceiver ? retainedReceiver(target, bindings, block)
                                                 : receiver(target, bindings, block);
        } else if (receiverMode_ != ReceiverMode::QualifiedPure &&
                   (call->resolution == "pure" || call->resolution == "effect")) {
            object = destination.retainsReceiver ? retainedSelf(call->offset, block)
                                                 : selfReceiver(call->offset, block);
        }
        auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, object);
        for (const auto& field : destination.workerFields)
            arguments.push_back(load(bindings.at(workerFieldParameter(field.declaration->name)), call->offset, block));
        auto abi = CallAbi::Scalar;
        if (function_.usesRuntime())
            abi = CallAbi::Contextual;
        else if (function_.construction())
            abi = CallAbi::Construction;
        const auto contextualResult = abi != CallAbi::Scalar ? type : std::nullopt;
        std::optional<Register> result;
        if (type)
            result = Register{registers_++};
        if (contextualResult)
            function_.childResultWords = std::max(function_.childResultWords, valueWords(*type, layouts_.view()));
        function_.blocks[block].instructions.push_back(
            {call->offset, DirectCall{result, destination.symbol, std::move(arguments), abi, contextualResult}});
        if (!result)
            return std::nullopt;
        return Operand{*type, *result};
    }

    std::optional<Operand> callDispatched(const Expr& call, std::optional<NativeType> type,
                                          const Bindings& bindings, std::size_t& block)
    {
        const auto object = value(call->children.at(0)->children.at(0), bindings, block);
        const auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, object);
        const auto next = newBlock();
        const auto abi = function_.usesRuntime() ? CallAbi::Contextual : CallAbi::Construction;
        std::optional<std::size_t> slot;
        if (type) {
            slot = function_.slots.size();
            function_.slots.push_back({{}, *type, call->offset});
            function_.childResultWords = std::max(function_.childResultWords, valueWords(*type, layouts_.view()));
        }
        dispatchReceiver(call, arguments.front(), block, [&](std::size_t target, const std::string& symbol) {
            std::optional<Register> result;
            if (type)
                result = Register{registers_++};
            auto& instructions = function_.blocks[target].instructions;
            instructions.push_back({call->offset, DirectCall{result, symbol, arguments, abi, type}});
            if (result)
                instructions.push_back({call->offset, Store{*slot, {*type, *result}, false}});
            terminate(target, call->offset, Jump{next});
        });
        block = next;
        if (slot)
            return load(*slot, call->offset, block);
        return std::nullopt;
    }

    Operand awaitExpression(const Expr& expression, const Bindings& bindings, std::size_t& block)
    {
        const auto& call = expression->children.at(0);
        if (call->resolution == "host")
            return awaitHostCall(expression, bindings, block);
        const bool dispatch = call->resolution == "source_interface_async" ||
            call->resolution == "class_virtual_async";
        const bool instance = dispatch || call->resolution == "object_async" || call->resolution == "local_async";
        if (!instance && call->resolution != "static_async")
            return awaitSemaphore(expression, bindings, block);

        const auto destination = dispatch ? std::optional<ResolvedCall>{} : resolve_(call);
        std::optional<Operand> object;
        if (call->resolution == "local_async") {
            object = destination->retainsReceiver ? retainedSelf(call->offset, block)
                                                  : selfReceiver(call->offset, block);
        } else if (instance) {
            const auto& target = call->children.at(0)->children.at(0);
            if (dispatch) {
                object = value(target, bindings, block);
            } else {
                budgetStep(block, target->offset);
                object = destination->retainsReceiver ? retainedReceiver(target, bindings, block)
                                                     : receiver(target, bindings, block);
            }
        }

        auto arguments = evaluateOperands(std::span(call->children).subspan(1), bindings, block, object);
        if (instance) {
            object = arguments.front();
            arguments.erase(arguments.begin());
        }

        std::vector<std::optional<std::size_t>> slots(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (!indirectArgument(argument.type))
                continue;
            slots[index] = function_.slots.size();
            function_.slots.push_back({{}, argument.type, expression->offset});
            function_.blocks[block].instructions.push_back(
                {expression->offset, Store{*slots[index], argument, false}});
        }
        const auto resume = newBlock();
        const auto suspend = [&](std::size_t target, const std::string& destination) {
            terminate(target, expression->offset, AwaitCall{destination, object, arguments, resume, slots});
        };
        if (dispatch)
            dispatchReceiver(call, *object, block, suspend);
        else
            suspend(block, destination->symbol);
        block = resume;
        const Register result{registers_++};
        const auto type = layouts_.type(expression->type, expression->offset);
        function_.childResultWords = std::max(function_.childResultWords, valueWords(type, layouts_.view()));
        function_.blocks[block].instructions.push_back({expression->offset, ResumedCall{result, type}});
        return {type, result};
    }

    template<class Exit>
    void terminate(std::size_t block, std::size_t offset, Exit exit)
    {
        auto& body = function_.blocks[block];
        body.exitOffset = offset;
        body.exit = std::move(exit);
    }

    void requireBinary32Environment(std::size_t block, std::size_t offset)
    {
        const Register result{registers_++};
        function_.blocks[block].instructions.push_back({offset,
            FloatingCall{result, native_app_v19::FloatingBuiltin::RequireBinary32Environment,
                         ScalarType::Float32, ScalarType::Integer, {}}});
    }

    void budgetStep(std::size_t block, std::size_t offset, std::size_t count = 1)
    {
        if ((function_.usesRuntime() || function_.construction()) && count != 0)
            function_.blocks[block].instructions.push_back({offset, BudgetStep{count}});
    }

    std::optional<std::size_t> conditionLoop(const Statement& statement, Bindings scope, std::size_t block)
    {
        const bool isFor = statement.kind == "for";
        if (isFor)
            block = scopedStatements(statement.initialization, scope, block, false).value();

        auto header = newBlock();
        const auto start = header;
        const auto body = newBlock();
        const auto repeat = isFor ? newBlock() : start;
        const auto exit = newBlock();
        terminate(block, statement.offset, Jump{start});
        const auto condition = value(statement.expression, scope, header);
        terminate(header, statement.offset, Branch{condition, body, exit});

        loops_.push_back({exit, repeat, protected_.size()});
        const auto end = statements(statement.body, scope, body);
        const auto hasBreak = loops_.back().hasBreak;
        loops_.pop_back();
        if (end)
            terminate(*end, statement.offset, Jump{repeat});
        if (isFor) {
            const auto iteration = statements(statement.iteration, scope, repeat);
            terminate(iteration.value(), statement.offset, Jump{start});
        }

        if (!hasBreak && statement.expression->kind == "boolean" && statement.expression->text == "true") {
            terminate(exit, statement.offset, Unreachable{});
            return std::nullopt;
        }
        return exit;
    }

    void lowerParallelInvocation(const Statement& statement, const Bindings& bindings, std::size_t& block)
    {
        const auto sourceItem = sequenceElementType(statement.expression->type).value();
        if (sourceItem != statement.type)
            throw Diagnostic("native parallel foreach requires the checked sequence element type", statement.offset);
        const auto worker = parallel_(statement);
        if (statement.parallelCaptures.size() + worker.fields.size() >= native_app_v19::maximumArguments)
            throw Diagnostic("native parallel foreach capture count exceeds the ABI limit", statement.offset);

        const auto sequence = collectionReceiver(statement.expression, bindings, block, true);
        if (layouts_.containsReference(sequence.type))
            throw Diagnostic("native parallel input requires owned sequence elements", statement.offset);
        const auto item = layouts_.type(statement.type, statement.offset);
        std::vector<Operand> captures;
        std::size_t captureWords = 0;
        const auto appendCapture = [&](Operand captured, std::size_t offset) {
            if (function_.usesRuntime() && captured.type == ScalarType::Semaphore) {
                const Register shared{registers_++};
                function_.blocks[block].instructions.push_back({offset, ShareSemaphore{shared, captured}});
                captured.value = shared;
            }
            if (captured.type != ScalarType::AtomicReference && captured.type != ScalarType::LockReference &&
                captured.type != ScalarType::Semaphore && captured.type != ScalarType::ConcurrentSequenceReference &&
                captured.type != ScalarType::ConcurrentDictionaryReference &&
                !layouts_.persistentValue(captured.type, true))
                throw Diagnostic("native parallel capture requires an owned value or a synchronization handle",
                                 offset);
            captureWords += valueWords(captured.type, layouts_.view());
            captures.push_back(captured);
        };
        for (const auto& capture : statement.parallelCaptures)
            appendCapture(load(bindings.at(capture.name), capture.offset, block), capture.offset);
        for (const auto& field : worker.fields) {
            const auto offset = statement.offset;
            const auto type = layouts_.type(field.declaration->type, offset);
            const auto receiver = selfReceiver(offset, block);
            appendCapture(readReceiverField(receiver, field.receiverOffset, type, offset, block), offset);
        }
        if (captureWords + valueWords(item, layouts_.view()) > native_app_v19::maximumFrameWords)
            throw Diagnostic("native parallel callback arguments exceed the storage limit", statement.offset);

        function_.parallelCaptureWords = std::max(function_.parallelCaptureWords,
                                                 valueWords(sequence.type, layouts_.view()) + captureWords);
        std::vector<Operand> cancellation;
        cancellation.reserve(parallelCancellationSlots_.size());
        for (const auto token : parallelCancellationSlots_)
            cancellation.push_back(load(token, statement.offset, block));
        function_.parallelCancellationSlots = cancellation.size();
        function_.blocks[block].instructions.push_back(
            {statement.offset, ParallelInvocation{sequence, item, worker.symbol,
                                                  std::move(captures), std::move(cancellation)}});
    }

    std::size_t forEach(const Statement& statement, Bindings scope, std::size_t block)
    {
        auto collection = collectionReceiver(statement.expression, scope, block, true);
        if (collection.type == ScalarType::HashSet) {
            const auto list = layouts_.type(Type{"List", statement.expression->type.arguments}, statement.offset);
            collection = setItems(collection, list, statement.offset, block);
        }
        const auto snapshotSlot = function_.slots.size();
        function_.slots.push_back({{}, collection.type, statement.offset});
        function_.blocks[block].instructions.push_back({statement.offset, Store{snapshotSlot, collection, false}});
        const auto count = collection.type == ScalarType::ByteBuffer ? byteLength(collection, block, statement.offset)
            : collectionCall(CollectionOperation::Count, ScalarType::Integer, collection, {}, statement.offset, block);
        const auto countSlot = function_.slots.size();
        function_.slots.push_back({{}, ScalarType::Integer, statement.offset});
        function_.blocks[block].instructions.push_back({statement.offset, Store{countSlot, count, false}});
        const auto indexSlot = function_.slots.size();
        function_.slots.push_back({{}, ScalarType::Integer, statement.offset});
        function_.blocks[block].instructions.push_back(
            {statement.offset, Store{indexSlot, {ScalarType::Integer, std::int64_t{0}}, false}});

        const auto sourceItem = sequenceElementType(statement.expression->type).value();
        const auto sourceItemType = layouts_.type(sourceItem, statement.offset);
        const auto itemType = layouts_.type(statement.type, statement.offset);
        const auto itemSlot = function_.slots.size();
        function_.slots.push_back({statement.name, itemType, statement.offset});
        scope.emplace(statement.name, itemSlot);

        const auto header = newBlock();
        const auto body = newBlock();
        const auto repeat = newBlock();
        const auto exit = newBlock();
        terminate(block, statement.offset, Jump{header});
        const auto index = load(indexSlot, statement.offset, header);
        const auto limit = load(countSlot, statement.offset, header);
        const Register inBounds{registers_++};
        function_.blocks[header].instructions.push_back({statement.offset, Compare{inBounds, Predicate::Less, index, limit}});
        terminate(header, statement.offset, Branch{{ScalarType::Boolean, inBounds}, body, exit});

        budgetStep(body, statement.offset);
        const auto currentSnapshot = load(snapshotSlot, statement.offset, body);
        const auto item = collection.type == ScalarType::ByteBuffer
            ? emitByteCall(native_app_v19::ByteOperation::At, sourceItemType, {currentSnapshot, index}, body, statement.offset)
            : collectionCall(CollectionOperation::At, sourceItemType, currentSnapshot, index, statement.offset, body);
        const auto converted = convertCheckedValue(item, sourceItem, statement.type, body, statement.offset);
        function_.blocks[body].instructions.push_back({statement.offset, Store{itemSlot, converted, true}});
        loops_.push_back({exit, repeat, protected_.size()});
        const auto end = statements(statement.body, scope, body);
        loops_.pop_back();
        if (end)
            terminate(*end, statement.offset, Jump{repeat});

        const auto current = load(indexSlot, statement.offset, repeat);
        const auto next = checkedInteger(IntegerOperation::Add, current, {ScalarType::Integer, std::int64_t{1}},
                                         repeat, statement.offset);
        function_.blocks[repeat].instructions.push_back({statement.offset, Store{indexSlot, next, false}});
        terminate(repeat, statement.offset, Jump{header});

        return exit;
    }

    std::optional<std::size_t> statements(std::span<const Statement> body, Bindings bindings,
                                           std::size_t block)
    {
        return scopedStatements(body, bindings, block, true);
    }

    void routeTransfer(std::size_t block, Transfer transfer, std::size_t offset)
    {
        if (protected_.size() == transfer.cleanupDepth) {
            terminate(block, offset, Jump{transfer.destination});
            return;
        }

        auto& region = protected_.back();
        const auto found = std::find(region.transfers.begin(), region.transfers.end(), transfer);
        const auto selector = static_cast<std::int64_t>(found - region.transfers.begin());
        if (found == region.transfers.end())
            region.transfers.push_back(transfer);
        function_.blocks[block].instructions.push_back(
            {offset, Store{region.selector, {ScalarType::Integer, selector}, false}});
        terminate(block, offset, Jump{region.entry});
    }

    void returnValue(std::size_t block, std::optional<Operand> value, std::size_t offset)
    {
        if (protected_.empty()) {
            terminate(block, offset, Return{value});
            return;
        }
        if (!returnBlock_) {
            const auto destination = newBlock();
            function_.blocks[destination].faultTarget = 0;
            if (value) {
                returnSlot_ = function_.slots.size();
                function_.slots.push_back({{}, function_.result, offset});
                terminate(destination, offset, Return{load(returnSlot_, offset, destination)});
            } else {
                terminate(destination, offset, Return{});
            }
            returnBlock_ = destination;
        }
        if (value)
            function_.blocks[block].instructions.push_back({offset, Store{returnSlot_, *value, false}});
        routeTransfer(block, {*returnBlock_, 0}, offset);
    }

    std::vector<std::size_t> protectedBody(const Statement& statement, const Bindings& bindings, std::size_t block)
    {
        const auto enclosingFault = faultTarget_;
        std::optional<std::size_t> handler;
        if (!statement.name.empty()) {
            handler = newBlock();
            faultTarget_ = *handler;
        }
        const auto body = newBlock();
        terminate(block, statement.offset, Jump{body});
        std::vector<std::size_t> exits;
        if (const auto end = statements(statement.body, bindings, body))
            exits.push_back(*end);
        faultTarget_ = enclosingFault;

        if (handler) {
            auto catchBindings = bindings;
            const auto errorSlot = function_.slots.size();
            function_.slots.push_back({statement.name, ScalarType::Error, statement.offset});
            catchBindings.emplace(statement.name, errorSlot);
            const Register error{registers_++};
            auto& instructions = function_.blocks[*handler].instructions;
            instructions.push_back({statement.offset, CaptureError{error}});
            instructions.push_back({statement.offset, Store{errorSlot, {ScalarType::Error, error}, true}});
            if (const auto end = statements(statement.alternative, std::move(catchBindings), *handler))
                exits.push_back(*end);
        }
        return exits;
    }

    void dispatchProtectedTransfers(std::size_t finish, const ProtectedRegion& region, std::size_t offset)
    {
        std::vector<std::size_t> destinations;
        for (const auto& transfer : region.transfers) {
            const auto destination = newBlock();
            routeTransfer(destination, transfer, offset);
            destinations.push_back(destination);
        }
        if (destinations.empty()) {
            terminate(finish, offset, Unreachable{});
        } else {
            const auto pending = load(region.selector, offset, finish);
            terminate(finish, offset, DispatchTransfer{pending, std::move(destinations)});
        }
    }

    std::optional<std::size_t> lockStatement(const Statement& statement, const Bindings& bindings, std::size_t block)
    {
        const auto lock = value(statement.expression, bindings, block);
        function_.blocks[block].instructions.push_back({statement.offset, AcquireLock{lock}});

        const auto outerFault = faultTarget_;
        const auto release = newBlock();
        const auto join = newBlock();
        const auto selector = function_.slots.size();
        function_.slots.push_back({{}, ScalarType::Integer, statement.offset});

        faultTarget_ = release;
        const auto body = newBlock();
        terminate(block, statement.offset, Jump{body});
        protected_.push_back({release, selector, {}});
        const auto exit = statements(statement.body, bindings, body);
        if (exit)
            routeTransfer(*exit, {join, protected_.size() - 1}, statement.offset);

        const auto region = std::move(protected_.back());
        protected_.pop_back();
        faultTarget_ = outerFault;
        function_.blocks[release].instructions.push_back({statement.offset, ReleaseLock{}});
        dispatchProtectedTransfers(release, region, statement.offset);
        if (exit)
            return join;

        terminate(join, statement.offset, Unreachable{});
        return std::nullopt;
    }

    std::optional<std::size_t> tryStatement(const Statement& statement, const Bindings& bindings, std::size_t block)
    {
        if (!statement.hasFinally) {
            const auto exits = protectedBody(statement, bindings, block);
            if (exits.empty())
                return std::nullopt;
            const auto join = newBlock();
            for (const auto exit : exits)
                terminate(exit, statement.offset, Jump{join});
            return join;
        }
        const auto outerFault = faultTarget_;
        const auto entry = newBlock();
        const auto finish = newBlock();
        const auto join = newBlock();
        const auto selector = function_.slots.size();
        function_.slots.push_back({{}, ScalarType::Integer, statement.offset});

        faultTarget_ = entry;
        protected_.push_back({entry, selector, {}});
        const auto exits = protectedBody(statement, bindings, block);
        for (const auto exit : exits)
            routeTransfer(exit, {join, protected_.size() - 1}, statement.offset);
        auto region = std::move(protected_.back());
        protected_.pop_back();

        faultTarget_ = finish;
        const auto cleanup = newBlock();
        function_.blocks[entry].instructions.push_back({statement.offset, BeginCleanup{}});
        terminate(entry, statement.offset, Jump{cleanup});
        const auto cleanupEnd = statements(statement.cleanup, bindings, cleanup);
        if (cleanupEnd)
            terminate(*cleanupEnd, statement.offset, Jump{finish});

        faultTarget_ = outerFault;
        function_.blocks[finish].instructions.push_back({statement.offset, EndCleanup{}});
        dispatchProtectedTransfers(finish, region, statement.offset);
        if (!exits.empty() && cleanupEnd)
            return join;
        terminate(join, statement.offset, Unreachable{});
        return std::nullopt;
    }

    std::optional<std::size_t> scopedStatements(std::span<const Statement> body, Bindings& bindings,
                                                 std::size_t block, bool ownsLocalBindings)
    {
        for (std::size_t statementIndex = 0; statementIndex < body.size(); ++statementIndex) {
            const auto& statement = body[statementIndex];
            budgetStep(block, statement.offset);
            if (statement.kind == "local") {
                std::optional<ReceiverAllocation> allocation;
                const auto initial = initializeLocal(statement, bindings, block, allocation);
                const auto index = function_.slots.size();
                if (ownsLocalBindings && !loops_.empty() && function_.usesRuntime() && !function_.construction() &&
                    synchronousHostCall(statement.expression) &&
                    statement.expression->type.name == "Result" && statement.expression->type.arguments.size() == 2 &&
                    statement.expression->type.arguments.front() == byteBufferType) {
                    auto& instruction = function_.blocks[block].instructions.back();
                    auto* call = std::get_if<HostCall>(&instruction.operation);
                    // Only a new evaluation of this lexical initializer can replace the bytes.
                    // Keep invocation ownership when the payload can be aliased or escape.
                    if (call && keepsHostBytesLocal(body.subspan(statementIndex + 1), statement.name))
                        call->reusableByteSlot = index;
                }
                if (allocation)
                    localReceivers_.emplace(index, *allocation);
                function_.slots.push_back({statement.name, initial.type, statement.offset});
                bindings.insert_or_assign(statement.name, index);
                function_.blocks[block].instructions.push_back({statement.offset, Store{index, initial, true}});
            } else if (statement.kind == "assign") {
                if ((function_.usesRuntime() || function_.construction()) && statement.target->resolution == "record") {
                    assignRecordField(statement, bindings, block);
                    continue;
                }
                if (receiverField(statement.target) || statement.target->resolution == "service") {
                    const auto& target = statement.target;
                    const auto field = layouts_.receiverField(target->type, target->offset);
                    const bool initializesSelf = function_.construction() &&
                        (target->kind != "member" || target->children.front()->kind == "this");
                    if (!initializesSelf && (field == ScalarType::Receiver || field == ScalarType::Interface))
                        assignsReceiverReference_ = true;
                    budgetLocation(target, target, block);
                    auto object = fieldReceiver(target, bindings, block);
                    const auto assigned = evaluateAfter(object, statement.expression, bindings, block);
                    function_.blocks[block].instructions.push_back(
                        {statement.offset, WriteField{object, fieldOffset(target), assigned, initializesSelf}});
                    continue;
                }
                const auto index = slot(statement.target, bindings);
                if (function_.slots.at(index).type == ScalarType::Receiver)
                    throw Diagnostic("native scalar objects cannot be copied or reassigned", statement.offset);

                budgetStep(block, statement.target->offset);
                const auto assigned = value(statement.expression, bindings, block);
                function_.blocks[block].instructions.push_back({statement.offset, Store{index, assigned, false}});
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && statement.expression->kind == "propagate") {
                budgetStep(block, statement.expression->offset);
                propagateResult(statement.expression, bindings, block);
            } else if (function_.continuation() && statement.kind == "expression" &&
                       statement.expression->kind == "await") {
                awaitExpression(statement.expression, bindings, block);
            } else if (statement.kind == "expression" &&
                       (statement.expression->resolution == "context_replace" ||
                        statement.expression->resolution == "context_invalidate" ||
                        statement.expression->resolution == "context_close")) {
                const auto& expression = statement.expression;
                if (!function_.usesRuntime() && !function_.construction())
                    throw Diagnostic("native ContextOwner mutation requires an invocation or construction", expression->offset);
                const auto& target = expression->children.at(0);
                const auto owner = value(target->children.at(0), bindings, block);
                using Operation = native_app_v19::ContextOwnerOperation;
                auto operation = Operation::Close;
                if (expression->resolution == "context_replace")
                    operation = Operation::Replace;
                else if (expression->resolution == "context_invalidate")
                    operation = Operation::Invalidate;
                function_.blocks[block].instructions.push_back(
                    {expression->offset, SynchronizationOperation{ContextOwnerCall{owner, operation}}});
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "expression" &&
                       statement.expression->resolution == "sync_release") {
                value(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "expression" &&
                       statement.expression->resolution == "sync_wait") {
                budgetStep(block, statement.expression->offset);
                waitSemaphore(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && concurrentDictionaryCall(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                callConcurrentDictionary(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && concurrentSequenceCall(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                callConcurrentSequence(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "expression" &&
                       statement.expression->resolution.starts_with("atomic_")) {
                const auto& expression = statement.expression;
                if (expression->type == voidType) {
                    budgetStep(block, expression->offset);
                    atomicCall(expression, bindings, block);
                } else {
                    value(expression, bindings, block);
                }
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && directSynchronousCall(statement.expression)) {
                const auto& call = statement.expression;
                budgetStep(block, call->offset);
                if (call->type == voidType)
                    callDirect(call, {}, bindings, block);
                else
                    unconvertedValue(call, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "expression" &&
                       (statement.expression->resolution == "source_interface_call" ||
                        statement.expression->resolution == "class_virtual_call")) {
                const auto& call = statement.expression;
                budgetStep(block, call->offset);
                if (call->type == voidType)
                    callDispatched(call, {}, bindings, block);
                else
                    unconvertedValue(call, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && hashSetCall(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                setCall(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && sequenceCallExpression(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                sequenceCall(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && dictionaryCallExpression(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                dictionaryCall(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "expression" &&
                       byteBufferMutation(statement.expression)) {
                budgetStep(block, statement.expression->offset);
                mutateBytes(statement.expression, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       statement.kind == "expression" && statement.expression->kind == "call" &&
                       (statement.expression->resolution == "list_add" ||
                        statement.expression->resolution == "collection_mutate_set")) {
                const auto& call = statement.expression;
                const auto& target = call->children.at(0)->children.at(0);
                budgetStep(block, call->offset);
                if (call->resolution == "list_add") {
                    auto place = collectionPlace(target, bindings, block);
                    const auto& item = call->children.at(1);
                    auto argument = place.object ? evaluateAfter(*place.object, item, bindings, block)
                                                 : value(item, bindings, block);
                    auto destination = readCollection(target, place, bindings, block);
                    const Register updated{registers_++};
                    function_.blocks[block].instructions.push_back(
                        {statement.offset, ListAppend{updated, destination.collection, argument}});
                    writeCollection(destination, {destination.collection.type, updated}, statement.offset, block);
                } else {
                    auto place = collectionPlace(target, bindings, block);
                    const auto operands = evaluateOperands(std::span(call->children).subspan(1), bindings, block,
                                                           place.object);
                    if (place.object)
                        place.object = operands.front();
                    auto destination = readCollection(target, place, bindings, block);
                    const auto first = operands.size() - 2;
                    const auto replacement = operands[first + 1];
                    const auto array = destination.collection;
                    const auto updated = array.type == ScalarType::FixedArray
                        ? fixedArrayAccess(array, operands[first], replacement.type, replacement, statement.offset, block)
                        : arrayCall(native_app_v19::ArrayOperation::Set, array.type, array, operands[first], replacement,
                                    array.type, statement.offset, block);
                    writeCollection(destination, updated, statement.offset, block);
                }
            } else if (statement.kind == "return") {
                if (function_.returnsVoid) {
                    returnValue(block, {}, statement.offset);
                } else {
                    const auto result = value(statement.expression, bindings, block);
                    returnValue(block, result, statement.offset);
                }
                return std::nullopt;
            } else if (statement.kind == "block") {
                const auto next = statements(statement.body, bindings, block);
                if (!next)
                    return std::nullopt;
                block = *next;
            } else if ((function_.usesRuntime() || function_.construction()) &&
                       (statement.kind == "while" || statement.kind == "for")) {
                const auto next = conditionLoop(statement, bindings, block);
                if (!next)
                    return std::nullopt;
                block = *next;
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "parallel_foreach") {
                lowerParallelInvocation(statement, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "foreach") {
                block = forEach(statement, bindings, block);
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "lock") {
                const auto next = lockStatement(statement, bindings, block);
                if (!next)
                    return std::nullopt;
                block = *next;
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "try") {
                const auto next = tryStatement(statement, bindings, block);
                if (!next)
                    return std::nullopt;
                block = *next;
            } else if ((function_.usesRuntime() || function_.construction()) && statement.kind == "throw") {
                const auto error = statement.expression ? value(statement.expression, bindings, block)
                    : load(bindings.at(statement.name), statement.offset, block);
                function_.blocks[block].instructions.push_back({statement.offset, RaiseError{error}});
                terminate(block, statement.offset, Unreachable{});
                return std::nullopt;
            } else if (statement.kind == "break" || statement.kind == "continue") {
                if (loops_.empty())
                    throw Diagnostic("native loop transfer is outside the admitted loop", statement.offset);
                auto& loop = loops_.back();
                const bool exits = statement.kind == "break";
                if (exits)
                    loop.hasBreak = true;
                routeTransfer(block, {exits ? loop.exit : loop.repeat, loop.cleanupDepth}, statement.offset);
                return std::nullopt;
            } else if (statement.kind == "if") {
                const auto condition = value(statement.expression, bindings, block);
                const auto whenTrue = newBlock();
                const auto whenFalse = newBlock();
                terminate(block, statement.offset, Branch{condition, whenTrue, whenFalse});

                const auto trueExit = statements(statement.body, bindings, whenTrue);
                const auto falseExit = statements(statement.alternative, bindings, whenFalse);
                if (!trueExit && !falseExit)
                    return std::nullopt;

                block = newBlock();
                if (trueExit)
                    terminate(*trueExit, statement.offset, Jump{block});
                if (falseExit)
                    terminate(*falseExit, statement.offset, Jump{block});
            } else {
                throw Diagnostic("unsupported native scalar statement: " + statement.kind, statement.offset);
            }
        }

        return block;
    }

    ScalarFunction function_;
    const ResolveCall& resolve_;
    const ResolveObject& object_;
    const ResolveDispatch& dispatch_;
    const ResolveParallel& parallel_;
    ValueLayouts& layouts_;
    std::set<std::size_t> activeDefaults_;
    std::map<std::size_t, ReceiverAllocation> localReceivers_;
    std::vector<LoopTargets> loops_;
    std::vector<ProtectedRegion> protected_;
    std::size_t faultTarget_ = 0;
    std::optional<std::size_t> returnBlock_;
    std::size_t returnSlot_ = 0;
    bool hasReceiver_;
    ReceiverMode receiverMode_;
    bool hostedBindingConstructor_;
    Binary32Checks binary32Checks_;
    std::vector<std::size_t> parallelCancellationSlots_;
    bool retainsReceiver_ = false;
    bool assignsReceiverReference_ = false;
    std::size_t registers_ = 0;
    std::size_t semaphoreSites_ = 0;
    std::unordered_set<std::string> escapingAtomics_;
    std::size_t localAtomicSites_ = 0;
};

std::string normalizedTypeList(std::string_view typeList)
{
    std::string types(typeList);
    std::erase_if(types, [](char character) {
        return character == ' ' || character == '\t';
    });
    return types;
}

std::string parameterTypeList(const Method& method)
{
    std::string types = "(";
    for (const auto& parameter : method.parameters) {
        if (types.size() > 1)
            types += ',';
        types += parameter.type.displayText();
    }
    types += ')';
    return normalizedTypeList(types);
}

struct SelectedMethod {
    const Declaration& owner;
    const Declaration& declaring;
    const Method& method;
    std::string name;
};

SelectedMethod selectEntry(const CheckedProgram& checked, std::string_view entry, bool hosted = false)
{
    const auto signatureStart = entry.find('(');
    const auto qualifiedName = entry.substr(0, signatureStart);
    const auto separator = qualifiedName.rfind('.');
    if (entry.size() > 1024 || separator == std::string_view::npos || separator == 0 ||
        separator + 1 == qualifiedName.size() || (signatureStart != std::string_view::npos && entry.back() != ')'))
        throw Diagnostic("select a native entry as Module.Class.Method or Module.Class.Method(parameter types)", 0);

    const auto ownerName = qualifiedName.substr(0, separator);
    const auto methodName = qualifiedName.substr(separator + 1);
    const auto signature = signatureStart == std::string_view::npos
        ? std::string{} : normalizedTypeList(entry.substr(signatureStart));
    const auto& declarations = checked.definition().declarations;
    const auto owner = std::find_if(declarations.begin(), declarations.end(), [&](const Declaration& declaration) {
        return declaration.kind == "Class" && declaration.sourceName == ownerName;
    });
    if (owner == declarations.end())
        throw Diagnostic("native entry class was not found", 0);
    if (!owner->typeParameters.empty())
        failAtOwner(*owner, "native entry requires a closed class", owner->offset);

    const Declaration* declaring = nullptr;
    const Method* selected = nullptr;
    std::set<std::string> availableSignatures;
    const auto admitMethods = [&](const Declaration& declaration) {
        for (const auto& method : declaration.methods) {
            if (method.name != methodName || !method.visible || method.staticMethod == hosted ||
                method.constructor || !method.typeParameters.empty())
                continue;
            auto parameters = parameterTypeList(method);
            if ((!signature.empty() && parameters != signature) ||
                !availableSignatures.insert(std::move(parameters)).second)
                continue;
            if (selected)
                failAtOwner(declaration, "native entry has multiple public overloads; select a parameter type list", method.offset);

            declaring = &declaration;
            selected = &method;
        }
    };

    admitMethods(*owner);
    for (const auto& ancestor : owner->ancestors) {
        const auto parent = std::find_if(declarations.begin(), declarations.end(), [&](const Declaration& declaration) {
            return declaration.name == ancestor;
        });
        if (parent == declarations.end())
            failAtOwner(*owner, "native entry has an unresolved checked ancestor", owner->offset);
        admitMethods(*parent);
    }
    if (!selected && !signature.empty())
        failAtOwner(*owner, "native entry has no public overload matching " + std::string(methodName) + signature,
                    owner->offset);
    if (!selected)
        failAtOwner(*owner, hosted ? "native hosted entry must be a public non-generic instance method"
                                   : "native entry must be a public static non-generic method", owner->offset);

    return {*owner, *declaring, *selected, std::string(methodName) + signature};
}

bool launcherParameters(const Method& method)
{
    const auto& parameters = method.parameters;
    if (parameters.empty())
        return true;
    const auto& firstType = parameters.front().type;
    const auto firstToken = firstType == Type{"int", {}} || firstType == processArgumentsType
        ? parameters.begin() + 1 : parameters.begin();
    return parameters.end() - firstToken <= 2 &&
        std::all_of(firstToken, parameters.end(), [](const Field& parameter) {
            return parameter.type == cancellationToken;
        });
}

const Method& selectHostedConstructor(const Declaration& owner, std::string_view requestedSignature)
{
    const auto signature = normalizedTypeList(requestedSignature);
    const Method* constructor = nullptr;
    for (const auto& candidate : owner.methods) {
        if (!candidate.constructor || !candidate.visible)
            continue;
        if (!signature.empty() && parameterTypeList(candidate) != signature)
            continue;
        if (constructor)
            failAtOwner(owner, "native hosted entry has multiple public constructors; select one with --host-constructor",
                        candidate.offset);
        constructor = &candidate;
    }
    if (!constructor && !signature.empty())
        failAtOwner(owner, "native hosted class has no public constructor with parameter types " + signature, owner.offset);
    if (!constructor || constructor->parameters.size() > native_app_v19::maximumArguments)
        failAtOwner(owner, "native hosted construction requires a public constructor with up to 32 arguments", owner.offset);
    return *constructor;
}

bool numericAbiType(NativeType type, std::span<const ValueLayout> layouts)
{
    if (type == ScalarType::Record) {
        const auto& fields = layouts[type.layout].fields;
        return !fields.empty() && std::all_of(fields.begin(), fields.end(),
            [layouts](const ValueField& field) { return numericAbiType(field.type, layouts); });
    }
    return type == ScalarType::Integer || type == ScalarType::UInt32 || isFloating(type);
}

[[noreturn]] void failNumericProjection(const ScalarFunction& function, std::string message, std::size_t offset)
{
    Diagnostic error(std::move(message), offset);
    error.path = function.sourceFile;
    throw error;
}

void projectNumericFunction(ScalarFunction& function, std::span<const ValueLayout> layouts)
{
    if (function.mode != FunctionMode::Contextual || function.returnsVoid || !numericAbiType(function.result, layouts))
        failNumericProjection(function, "native numeric export requires a primitive numeric or nonempty numeric record return type", function.offset);
    for (const auto& parameter : std::span(function.slots).first(function.parameterCount)) {
        if (!numericAbiType(parameter.type, layouts))
            failNumericProjection(function, "native numeric export requires primitive numeric or nonempty numeric record parameters", parameter.offset);
    }
    for (const auto& local : std::span(function.slots).subspan(function.parameterCount)) {
        if (!numericAbiType(local.type, layouts) && local.type != ScalarType::Boolean)
            failNumericProjection(function, "native numeric export requires primitive numeric, nonempty numeric record, or bool locals", local.offset);
    }
    for (auto& block : function.blocks) {
        if (block.faultTarget != 0 || !block.exit ||
            !(std::holds_alternative<Jump>(*block.exit) || std::holds_alternative<Branch>(*block.exit) ||
              std::holds_alternative<Return>(*block.exit) || std::holds_alternative<Unreachable>(*block.exit))) {
            failNumericProjection(function, "unsupported control flow in native numeric export", block.exitOffset);
        }

        // Admission already proves bounded evaluation for the entire reachable closure.
        std::erase_if(block.instructions, [](const Instruction& instruction) {
            return std::holds_alternative<BudgetStep>(instruction.operation);
        });
        for (auto& instruction : block.instructions) {
            auto& operation = instruction.operation;
            if (auto* direct = std::get_if<DirectCall>(&operation)) {
                if (direct->abi != CallAbi::Contextual || !direct->resultType ||
                    !numericAbiType(*direct->resultType, layouts)) {
                    failNumericProjection(function, "unsupported call in native numeric export", instruction.offset);
                }
                direct->abi = CallAbi::Scalar;
                if (*direct->resultType != ScalarType::Record)
                    direct->resultType.reset();
                continue;
            }
            if (const auto* builtin = std::get_if<FloatingCall>(&operation)) {
                using Builtin = native_app_v19::FloatingBuiltin;
                if (builtin->operation != Builtin::Narrow && builtin->operation != Builtin::Widen &&
                    builtin->operation != Builtin::FromInt) {
                    failNumericProjection(function, "unsupported floating operation in native numeric export",
                                          instruction.offset);
                }
                continue;
            }
            if (!(std::holds_alternative<Load>(operation) || std::holds_alternative<Store>(operation) ||
                  std::holds_alternative<Compare>(operation) || std::holds_alternative<FloatingCompute>(operation) ||
                  std::holds_alternative<ConstructRecord>(operation) || std::holds_alternative<ReadRecordField>(operation) ||
                  std::holds_alternative<UpdateRecordField>(operation))) {
                failNumericProjection(function, "unsupported operation in native numeric export", instruction.offset);
            }
        }
    }
    function.mode = FunctionMode::Scalar;
}

class ClosedCalls final {
public:
    explicit ClosedCalls(const CheckedProgram& checked)
        : definition_(checked.definition()), declarations_(definition_.declarations), layouts_(declarations_)
    {
    }

    ScalarProgram lower(const Declaration& owner, const Method& method, std::string name, FunctionMode mode)
    {
        runtimeProgram_ = mode != FunctionMode::Scalar;
        symbols_.insert(name);
        emit(owner, method, name, name, mode, true);
        if (hasInvocationRecursion_ || hasHostedRecursion_)
            validateRecursiveStack();
        return {std::move(functions_), layouts_.take(), {}, hasInvocationRecursion_, hasHostedRecursion_};
    }

    ScalarProgram lowerNumeric(const SelectedMethod& selected, std::string symbol)
    {
        runtimeProgram_ = true;
        const auto& owner = selected.declaring;
        const auto name = owner.sourceName + '.' + selected.name;
        constexpr auto entry = "pipelang_native_numeric_entry";
        symbols_.insert(entry);
        emit(owner, selected.method, name, entry, FunctionMode::Contextual, false, ReceiverMode::QualifiedPure);
        for (auto& function : functions_)
            projectNumericFunction(function, layouts_.view());

        ScalarProgram program{std::move(functions_), layouts_.take()};
        program.numericExport = std::move(symbol);
        return program;
    }

    ScalarProgram lowerHosted(const SelectedMethod& primary, const Method& constructor, std::string symbol,
                              std::span<const SelectedMethod> methods, HostedOptions options)
    {
        const auto& owner = primary.owner;
        const auto primaryName = primary.declaring.sourceName + '.' + primary.name;
        // Other selected methods can call the primary before its host entry is lowered.
        // Keep their source-call variant distinct from the host entry.
        symbols_.insert(primaryName);
        hostedOwner_ = &owner;
        usesPortableBinary32_ = !options.activeHostFloating && programUsesType(definition_, "float32");
        hostedObjectReferences_ = options.exposesObjectReferences;
        hostedGraph_ = hostedObjectReferences_;
        scalarObject(owner, owner.name, owner.offset, FunctionMode::Construction);
        std::vector<HostedParameter> parameters;
        for (const auto& parameter : constructor.parameters) {
            if (layouts_.service(parameter.type)) {
                parameters.push_back({ScalarType::Service, layouts_.declaration(parameter.type)->sourceName});
                continue;
            }
            const auto type = layouts_.type(parameter.type, parameter.offset);
            if (!layouts_.persistentValue(type) && !hasImplicitSharedHandle(parameter.type))
                failAtOwner(owner, "native hosted constructor requires a persistent value, shared handle or bound host service",
                            parameter.offset);
            parameters.push_back({type, {}});
        }
        const auto constructorSymbol = owner.sourceName + ".$hostConstructor";
        symbols_.insert(constructorSymbol);
        emit(owner, constructor, owner.sourceName + ".Constructor", constructorSymbol, FunctionMode::Construction, false,
             ReceiverMode::Ordinary, /*hostedBindingConstructor=*/true);
        const auto receiverWords = layouts_.receiverOffset(owner, owner.fields.size());
        auto recipes = options.composition ? compositionRecipes(*options.composition) : std::vector<HostedRecipeEntry>{};

        runtimeProgram_ = true;
        std::vector<HostedMethodEntry> entries;
        for (const auto& additional : methods) {
            const auto fullName = additional.declaring.sourceName + '.' + additional.name;
            const auto internalName = fullName + "$hosted";
            symbols_.insert(internalName);
            emit(additional.declaring, additional.method, fullName, internalName, FunctionMode::Continuation, true);
            functions_.back().exported = false;
            entries.push_back({additional.name, functions_.size() - 1});
        }
        std::optional<HostedSnapshot> snapshot;
        if (options.exposesSnapshot) {
            snapshot = HostedSnapshot{owner.sourceName + ".State", {}};
            for (std::size_t index = 0; index < owner.fields.size(); ++index) {
                const auto& field = owner.fields[index];
                if (!field.visible || layouts_.service(field.type))
                    continue;
                const auto type = layouts_.type(field.type, field.offset);
                if (!layouts_.persistentValue(type) || layouts_.containsReference(type))
                    failAtOwner(owner, "native hosted snapshots require owned fields", field.offset);
                snapshot->fields.push_back({field.name, type, layouts_.receiverOffset(owner, index)});
            }
        }
        auto composition = options.composition ? compositionPlan(*options.composition) : std::vector<HostedCompositionService>{};
        auto program = lower(primary.declaring, primary.method, primaryName, FunctionMode::Continuation);
        program.functions.back().exported = false;
        if (!entries.empty())
            entries.insert(entries.begin(), {primary.name, program.functions.size() - 1});
        program.hosted = HostedEntry{std::move(symbol), constructorSymbol, std::move(parameters), receiverWords,
                                     layouts_.receiverTag(owner), hostedGraph_, std::move(entries), std::move(snapshot),
                                     options.allowsMultipleTasks, options.exposesObjectReferences,
                                     std::move(recipes), std::move(composition)};
        return program;
    }

private:
    bool ownedCompositionValue(NativeType type) const
    {
        switch (type.kind) {
        case ScalarType::Boolean:
        case ScalarType::Integer:
        case ScalarType::UInt32:
        case ScalarType::Enum:
        case ScalarType::Float:
        case ScalarType::Float32:
        case ScalarType::Text:
        case ScalarType::ByteBuffer:
            return true;
        case ScalarType::List:
        case ScalarType::Array:
        case ScalarType::HashSet:
        case ScalarType::Queue:
        case ScalarType::Stack:
        case ScalarType::Dictionary:
        case ScalarType::Pair:
        case ScalarType::Record:
            return std::all_of(layouts_[type].fields.begin(), layouts_[type].fields.end(), [&](const auto& field) {
                return ownedCompositionValue(field.type);
            });
        default:
            return false;
        }
    }

    bool retainedCompositionValue(NativeType type) const
    {
        return type == ScalarType::ClassReference || type == ScalarType::Interface || ownedCompositionValue(type);
    }

    std::vector<HostedRecipeEntry> compositionRecipes(const CompositionGraph& composition)
    {
        const auto reject = [](const CompositionOrigin& origin, std::string message) -> void {
            Diagnostic error(std::move(message), origin.offset);
            error.path = origin.path;
            throw error;
        };
        const std::vector<CompositionGraph> selectedGraphs{composition};
        CompositionFactoryProfile profile(definition_, selectedGraphs);
        std::vector<HostedRecipeEntry> recipes;
        for (const auto index : composition.constructionOrder) {
            const auto& service = composition.services.at(index);
            if (service.input != CompositionInput::None)
                continue;

            const auto* implementation = layouts_.declaration(service.implementation);
            if (!implementation)
                reject(service.origin, "native hosted construction recipe has no source implementation");
            const auto selected = [&]() -> CompositionMethod {
                if (service.factory) {
                    const auto& factory = *service.factory;
                    const auto& receiver = composition.services.at(factory.receiverIndex);
                    const auto* concrete = layouts_.declaration(receiver.implementation);
                    if (receiver.input != CompositionInput::None || !concrete)
                        failAtOwner(*implementation, "native hosted factories require a constructed source receiver", factory.origin.offset);
                    return compositionMethod(*concrete, profile.checkedMethod(factory.identity));
                }
                const auto constructor = std::find_if(implementation->methods.begin(), implementation->methods.end(),
                    [&](const auto& candidate) { return candidate.constructor && candidate.identity == service.constructorIdentity; });
                if (constructor == implementation->methods.end())
                    failAtOwner(*implementation, "native hosted construction recipe has no checked constructor", service.constructorOrigin.offset);
                return {implementation, &*constructor};
            }();
            const auto& method = *selected.method;

            // The receiver can retain values that need no attempt-local
            // storage. Native object references belong to the binding's graph.
            for (auto current = implementation; current; current = current->baseClass.empty()
                     ? nullptr : layouts_.declaration(Type{current->baseClass, {}})) {
                for (const auto& field : current->fields) {
                    if (!layouts_.service(field.type) && !hasImplicitSharedHandle(field.type) &&
                        !retainedCompositionValue(layouts_.type(field.type, field.offset)))
                        failAtOwner(*current, "native hosted construction recipe requires owned receiver fields, same-graph classes or borrowed host ports",
                                    field.offset);
                }
            }

            std::vector<HostedParameter> parameters;
            std::optional<std::size_t> cancellationIndex;
            if (service.factory) {
                const auto& factoryReceiver = composition.services.at(service.factory->receiverIndex);
                parameters.push_back({layouts_.type(factoryReceiver.implementation, method.offset), {}});
            }
            for (const auto& parameter : method.parameters) {
                const auto index = parameters.size();
                if (service.factory && parameter.type == cancellationToken) {
                    cancellationIndex = index;
                    continue;
                }
                if (layouts_.service(parameter.type)) {
                    parameters.push_back({ScalarType::Service, layouts_.declaration(parameter.type)->sourceName});
                } else {
                    auto type = layouts_.type(parameter.type, parameter.offset);
                    if (type == ScalarType::ClassReference || type == ScalarType::Interface) {
                        const auto& dependency = composition.services.at(service.dependencies.at(index).target);
                        if (dependency.input != CompositionInput::None)
                            failAtOwner(*implementation, "native scoped reference dependency requires a constructed source service",
                                        parameter.offset);
                        // The checked graph supplies the concrete reference contract at this ABI boundary.
                        // The constructor still receives the same source receiver pointer.
                        type = layouts_.type(dependency.implementation, parameter.offset);
                    }
                    if (!retainedCompositionValue(type)) {
                        failAtOwner(*implementation, "native hosted construction recipe requires owned snapshots, same-graph classes or borrowed host ports",
                                    parameter.offset);
                    }
                    parameters.push_back({type, {}});
                }
            }

            const auto& receiver = scalarObject(*implementation, implementation->name, method.offset, FunctionMode::Construction);
            const auto callable = compileCall(receiver, *selected.owner, method.identity, FunctionMode::Construction,
                                              !service.factory, false, method.offset);
            const auto* contract = layouts_.declaration(service.service);
            if (!contract)
                failAtOwner(receiver, "native hosted construction recipe has no checked service type", service.origin.offset);
            std::string cleanup;
            if (service.cleanupIndex) {
                const auto& selector = composition.cleanupContracts.at(*service.cleanupIndex);
                const auto concrete = compositionMethod(receiver, profile.checkedMethod(selector.identity));
                cleanup = compileCall(receiver, *concrete.owner, concrete.method->identity,
                                      FunctionMode::Construction, false, false, concrete.method->offset).symbol;
            }

            const auto receiverType = layouts_.type(service.implementation, method.offset);
            recipes.push_back({contract->sourceName, callable.symbol, service.factory.has_value(), cancellationIndex, receiverType,
                               std::move(parameters), layouts_.receiverOffset(receiver, receiver.fields.size()),
                               layouts_.receiverTag(receiver), std::move(cleanup)});
        }
        if (recipes.empty())
            reject(composition.origin, "native hosted composition has no constructor recipes");
        return recipes;
    }

    CompositionMethod compositionMethod(const Declaration& implementation, const CompositionMethod& contract) const
    {
        const auto& signature = *contract.method;
        if (!contract.owner->sourceInterface && !signature.virtualMethod)
            return contract;

        for (auto owner = &implementation; owner; owner = owner->baseClass.empty()
                 ? nullptr : layouts_.declaration(Type{owner->baseClass, {}})) {
            const auto selected = std::find_if(owner->methods.begin(), owner->methods.end(), [&](const auto& candidate) {
                return std::ranges::find(signature.dispatchTargets, candidate.identity) != signature.dispatchTargets.end();
            });
            if (selected != owner->methods.end())
                return {owner, &*selected};
        }
        failAtOwner(implementation, "native composition method lost its checked concrete receiver target", implementation.offset);
    }

    std::vector<HostedMethodEntry> compositionMethods(const Type& service, const Declaration& implementation,
        CompositionFactoryProfile& profile, std::map<std::string, std::size_t>& methodFunctions)
    {
        const auto methods = profile.methods(service);
        if (methods.size() > native_app_v19::maximumHostedMethods)
            failAtOwner(implementation, "native hosted composition exceeds the method count limit", implementation.offset);

        std::vector<HostedMethodEntry> result;
        for (const auto& slot : methods) {
            const auto entry = compositionMethod(implementation, slot);
            const auto& method = *entry.method;
            const auto [function, inserted] = methodFunctions.emplace(method.identity, 0);
            if (inserted) {
                const auto symbol = method.identity + "$nativeComposition";
                const auto name = entry.owner->sourceName + '.' + method.name;
                symbols_.insert(symbol);
                emit(*entry.owner, method, name, symbol, FunctionMode::Continuation, true,
                     ReceiverMode::Ordinary, /*hostedBindingConstructor=*/false, /*preserveSynchronousResult=*/true);
                functions_.back().exported = false;
                function->second = functions_.size() - 1;
            }
            result.push_back({slot.method->identity.substr(slot.owner->name.size() + 1), function->second});
        }
        return result;
    }

    std::vector<HostedCompositionVariant> factoryVariants(const CompositionGraph& graph, const CompositionService& service,
        bool exported, CompositionFactoryProfile& profile, std::map<std::string, std::size_t>& methodFunctions)
    {
        std::vector<HostedCompositionVariant> result;
        for (const auto& candidate : definition_.declarations) {
            if (std::ranges::find(candidate.ancestors, service.implementation.name) == candidate.ancestors.end())
                continue;
            auto methods = exported ? compositionMethods(service.service, candidate, profile, methodFunctions)
                                    : std::vector<HostedMethodEntry>{};
            std::string cleanup;
            if (service.cleanupIndex) {
                const auto& selector = graph.cleanupContracts.at(*service.cleanupIndex);
                const auto selected = compositionMethod(candidate, profile.checkedMethod(selector.identity));
                cleanup = compileCall(candidate, *selected.owner, selected.method->identity,
                                      FunctionMode::Construction, false, false, selected.method->offset).symbol;
            }
            result.push_back({layouts_.receiverTag(candidate), layouts_.receiverOffset(candidate, candidate.fields.size()),
                              std::move(methods), std::move(cleanup)});
        }
        return result;
    }

    std::vector<HostedCompositionService> compositionPlan(const CompositionGraph& graph)
    {
        using native_app_v19::HostedLifetime;
        using native_app_v19::HostedCompositionInput;

        std::vector<HostedCompositionService> result;
        const std::vector<CompositionGraph> selected{graph};
        CompositionFactoryProfile facade(definition_, selected);
        std::vector<std::size_t> positions(graph.services.size());
        std::vector<bool> exported(graph.services.size());
        std::map<std::string, std::size_t> methodFunctions;
        for (const auto index : graph.exports)
            exported.at(index) = true;

        std::size_t recipe = 0;
        result.reserve(graph.services.size());
        for (const auto index : graph.constructionOrder) {
            const auto& service = graph.services.at(index);
            const auto* declaration = layouts_.declaration(service.service);
            auto lifetime = HostedLifetime::Transient;
            switch (service.lifetime) {
            case ServiceLifetime::Singleton: lifetime = HostedLifetime::Singleton; break;
            case ServiceLifetime::Scoped: lifetime = HostedLifetime::Scoped; break;
            case ServiceLifetime::Transient: break;
            }
            auto input = HostedCompositionInput::None;
            std::optional<NativeType> snapshot;
            if (service.input == CompositionInput::Snapshot) {
                input = HostedCompositionInput::Snapshot;
                snapshot = layouts_.type(service.service, service.origin.offset);
            } else if (service.input == CompositionInput::Borrowed) {
                input = HostedCompositionInput::Borrowed;
            }
            const auto flags = (exported[index] ? native_app_v19::hostedCompositionExported : 0) |
                (service.rootResolvable ? native_app_v19::hostedCompositionRootResolvable : 0) |
                (service.scopeResolvable ? native_app_v19::hostedCompositionScopeResolvable : 0);
            const auto name = declaration ? declaration->sourceName : service.service.displayText();
            HostedCompositionService node{name, static_cast<std::uint64_t>(lifetime),
                                          static_cast<std::uint64_t>(input), flags,
                                          input == HostedCompositionInput::None ? ++recipe : 0, {}, snapshot, {}, {}};
            if (exported[index] && input == HostedCompositionInput::None) {
                const auto* implementation = layouts_.declaration(service.implementation);
                node.methods = compositionMethods(service.service, *implementation, facade, methodFunctions);
            }
            if (service.factory)
                node.variants = factoryVariants(graph, service, exported[index], facade, methodFunctions);
            for (const auto& dependency : service.dependencies)
                node.dependencies.push_back(positions.at(dependency.target));
            positions[index] = result.size();
            result.push_back(std::move(node));
        }
        return result;
    }

    struct CallCost {
        std::size_t unmeteredWork;
        std::size_t stackWords;
        std::size_t depth;
        bool retainsReceiver;
    };

    struct CompiledCall {
        std::string symbol;
        CallCost cost;
        std::vector<WorkerField> workerFields;
    };

    std::vector<WorkerField> workerFields(const Declaration& owner, std::span<const Statement> body)
    {
        std::vector<bool> used(owner.fields.size());
        std::unordered_set<std::string> visited;
        std::vector<const Method*> pending;
        const auto inspectExpression = [&](auto&& inspect, const Expr& expression) -> void {
            if (!expression)
                return;
            if (expression->resolution == "field") {
                const auto fields = std::span(owner.fields);
                const auto field = std::ranges::find(fields, expression->text, &Field::name);
                if (field == fields.end() || expression->owner != owner.name)
                    failAtOwner(owner, "native worker cannot capture an external receiver field", expression->offset);
                used[static_cast<std::size_t>(field - fields.begin())] = true;
            }
            if (expression->kind == "call" && receiverFreeWorkerCall(expression) &&
                !expression->statelessWorkerCall && visited.insert(expression->callable).second) {
                const auto method = std::ranges::find(owner.methods, expression->callable, &Method::identity);
                if (method == owner.methods.end() || !method->workerCallable)
                    failAtOwner(owner, "native worker call lacks a checked helper target", expression->offset);
                pending.push_back(&*method);
            }
            for (const auto& child : expression->children)
                inspect(inspect, child);
        };
        const auto inspectStatements = [&](auto&& inspect, std::span<const Statement> statements) -> void {
            for (const auto& statement : statements) {
                inspectExpression(inspectExpression, statement.expression);
                inspectExpression(inspectExpression, statement.target);
                inspect(inspect, statement.initialization);
                inspect(inspect, statement.iteration);
                inspect(inspect, statement.body);
                inspect(inspect, statement.alternative);
                inspect(inspect, statement.cleanup);
            }
        };
        inspectStatements(inspectStatements, body);
        while (!pending.empty()) {
            const auto* method = pending.back();
            pending.pop_back();
            inspectStatements(inspectStatements, method->body);
        }

        std::vector<WorkerField> fields;
        for (std::size_t index = 0; index < used.size(); ++index) {
            if (used[index])
                fields.push_back({&owner.fields[index], layouts_.receiverOffset(owner, index)});
        }
        return fields;
    }

    static void appendWorkerParameters(Method& method, std::span<const WorkerField> fields)
    {
        for (const auto& field : fields) {
            const auto& declaration = *field.declaration;
            method.parameters.push_back({declaration.type, workerFieldParameter(declaration.name),
                                         true, declaration.offset});
        }
    }

    static constexpr std::size_t maximumLoweredOperations = 65536;
    static constexpr std::size_t maximumUnmeteredWork = 4096;
    // Leave room for target ABI argument lowering, spills and the SDK's own stack frames.
    static constexpr std::size_t maximumExplicitStackWords = native_app_v19::maximumFrameWords * 4;

    const Declaration& declaringClass(const Declaration& caller, std::string_view name, std::size_t offset) const
    {
        const auto owner = std::find_if(declarations_.begin(), declarations_.end(), [name](const auto& declaration) {
            return declaration.name == name;
        });
        if (owner == declarations_.end())
            failAtOwner(caller, "native call or field is missing its checked declaring class", offset);
        return *owner;
    }

    const Declaration& scalarObject(const Declaration& caller, std::string_view name, std::size_t offset,
                                    FunctionMode mode)
    {
        const auto& owner = declaringClass(caller, name, offset);
        if (owner.kind != "Class" || owner.sourceInterface || (owner.modelExported && &owner != hostedOwner_) ||
            !owner.typeParameters.empty() || !owner.genericSourceName.empty())
            failAtOwner(caller, "native receivers require an ordinary closed class", offset);
        if (!owner.baseClass.empty())
            scalarObject(owner, owner.baseClass, offset, mode);
        for (const auto& field : owner.fields) {
            if (field.type == Type{"int", {}} || field.type == Type{"bool", {}} || layouts_.service(field.type))
                continue;
            if (!hostedOwner_ && owner.interfaces.empty() && !runtimeProgram_)
                failAtOwner(owner, "native receiver field '" + field.name + "' requires int, bool or a bound host service",
                            field.offset);
            const auto type = layouts_.receiverField(field.type, field.offset);
            const bool durableSynchronization = type == ScalarType::AtomicReference ||
                type == ScalarType::LockReference || type == ScalarType::Semaphore ||
                type == ScalarType::ConcurrentSequenceReference || type == ScalarType::ConcurrentDictionaryReference ||
                type == ScalarType::ContextOwnerReference;
            if (hostedOwner_ && (isReceiverValue(type) || type == ScalarType::Interface) &&
                containsSourceReferences(type, layouts_.view())) {
                hostedGraph_ = true;
                continue;
            }
            if (type != ScalarType::Receiver && type != ScalarType::Interface && !durableSynchronization &&
                !layouts_.persistentValue(type, true) &&
                (mode == FunctionMode::Construction || !layouts_.invocationField(type)))
                failAtOwner(owner, "native hosted field '" + field.name + "' requires a persistent value or receiver",
                            field.offset);
        }
        if (layouts_.receiverOffset(owner, owner.fields.size()) > native_app_v19::maximumReceiverWords)
            failAtOwner(owner, "native receiver layout exceeds the storage limit", offset);
        return owner;
    }

    CompiledCall resolve(const Declaration& caller, FunctionMode callerMode, const Expr& call,
                         ReceiverMode callerReceiver)
    {
        const bool constructor = call->resolution == "object_construct";
        const bool asynchronous = call->resolution == "static_async" || call->resolution == "object_async" ||
            call->resolution == "local_async";
        const bool staticCall = call->resolution == "static_call" || call->resolution == "static_async";
        auto receiverMode = ReceiverMode::Ordinary;
        if (call->resolution == "qualified_pure" ||
            (callerReceiver == ReceiverMode::QualifiedPure && call->resolution == "pure")) {
            receiverMode = ReceiverMode::QualifiedPure;
        } else if (callerReceiver == ReceiverMode::ParallelWorker && receiverFreeWorkerCall(call)) {
            receiverMode = ReceiverMode::ParallelWorker;
        }
        const auto& owner = staticCall || receiverMode != ReceiverMode::Ordinary
            ? declaringClass(caller, call->owner, call->offset)
            : scalarObject(caller, call->owner, call->offset, callerMode);
        auto mode = FunctionMode::Scalar;
        if (asynchronous)
            mode = FunctionMode::Continuation;
        else if (callerMode == FunctionMode::Construction)
            mode = FunctionMode::Construction;
        else if (constructor && callerMode != FunctionMode::Scalar)
            mode = FunctionMode::InvocationConstruction;
        else if (!constructor && usesInvocationRuntime(callerMode))
            mode = FunctionMode::Contextual;

        if (call->targets.size() != 1)
            failAtOwner(caller, "native direct calls require one checked target", call->offset);
        return compileCall(caller, owner, call->callable, mode, constructor, staticCall, call->offset, receiverMode);
    }

    CompiledCall compileCall(const Declaration& caller, const Declaration& owner, const std::string& identity,
                             FunctionMode mode, bool constructor, bool staticCall, std::size_t offset,
                             ReceiverMode receiverMode = ReceiverMode::Ordinary)
    {
        const auto variant = std::pair{identity, receiverMode};
        const auto key = std::pair{variant, mode};
        const auto existing = compiled_.find(key);
        if (active_.contains(variant) && mode != FunctionMode::Continuation) {
            if (mode == FunctionMode::Scalar || existing == compiled_.end())
                failAtOwner(caller, "native direct calls cannot be recursive outside an invocation", offset);
            if (mode == FunctionMode::Construction)
                hasHostedRecursion_ = true;
            else
                hasInvocationRecursion_ = true;
        }
        if (existing != compiled_.end())
            return existing->second;
        if (active_.size() >= model::SourceCallContext::maximumDepth)
            failAtOwner(caller, "native call path exceeds the source call depth limit", offset);

        const auto method = std::find_if(owner.methods.begin(), owner.methods.end(), [&](const auto& candidate) {
            return candidate.identity == identity;
        });
        if (method == owner.methods.end() || method->constructor != constructor ||
            method->staticMethod != staticCall || method->asynchronous != (mode == FunctionMode::Continuation) ||
            !method->typeParameters.empty())
            failAtOwner(caller, "native direct calls require a closed checked method", offset);
        if (receiverMode == ReceiverMode::ParallelWorker) {
            const bool checkedWorker = method->statelessWorkerCallable || method->workerCallable;
            if (!checkedWorker || mode != FunctionMode::Contextual)
                failAtOwner(caller, "native parallel helper requires a checked receiver-free method", offset);
        }
        if (mode == FunctionMode::Scalar && !constructor && (!method->pure || method->result != Type{"int", {}}))
            failAtOwner(caller, "native synchronous calls require a pure method returning int", offset);

        if (receiverMode == ReceiverMode::QualifiedPure &&
            (!owner.modelExported || !owner.fields.empty() || !method->pure || staticCall || constructor || method->asynchronous))
            failAtOwner(caller, "native qualified call requires a checked fieldless pure method", offset);

        const auto& methodName = method->sourceName.empty() ? method->name : method->sourceName;
        const auto name = owner.sourceName + '.' + methodName;
        auto symbol = receiverMode == ReceiverMode::ParallelWorker ? name + "$worker" : name;
        if (!symbols_.insert(symbol).second)
            symbol += "$native" + std::to_string(nextSymbol_++);
        // Back edges reference the reserved symbol. Calls check their dynamic
        // ancestry in the invocation, where it also includes async frames.
        // A recursive instance edge can retain self until its summary is known.
        const bool retainsReceiver = !staticCall && receiverMode == ReceiverMode::Ordinary && usesInvocationRuntime(mode);
        auto fields = receiverMode == ReceiverMode::ParallelWorker && method->workerCallable
            ? workerFields(owner, method->body) : std::vector<WorkerField>{};
        const auto [compiled, inserted] = compiled_.emplace(
            key, CompiledCall{std::move(symbol), {0, 0, 0, retainsReceiver}, std::move(fields)});
        (void)inserted;
        if (compiled->second.workerFields.empty()) {
            compiled->second.cost = emit(owner, *method, name, compiled->second.symbol, mode, false, receiverMode);
        } else {
            auto worker = *method;
            appendWorkerParameters(worker, compiled->second.workerFields);
            compiled->second.cost = emit(owner, worker, name, compiled->second.symbol, mode, false, receiverMode);
        }
        return compiled->second;
    }

    CallCost emit(const Declaration& owner, const Method& method, const std::string& name,
                  const std::string& symbol, FunctionMode mode, bool exported,
                  ReceiverMode receiverMode = ReceiverMode::Ordinary, bool hostedBindingConstructor = false,
                  bool preserveSynchronousResult = false)
    {
        const auto active = active_.emplace(method.identity, receiverMode);
        const bool metered = usesInvocationRuntime(mode) || mode == FunctionMode::Construction;
        std::size_t unmeteredWork = 0;
        std::size_t calleeStackWords = 0;
        std::size_t calleeDepth = 0;
        const auto account = [&](const Expr& expression, const CompiledCall& callee, bool asynchronous) {
            if (!metered) {
                if (callee.cost.unmeteredWork > maximumUnmeteredWork - unmeteredWork)
                    failAtOwner(owner, "native unmetered calls exceed the bounded evaluation cost", expression->offset);
                unmeteredWork += callee.cost.unmeteredWork;
            }
            if (callee.cost.depth >= model::SourceCallContext::maximumDepth)
                failAtOwner(owner, "native call path exceeds the source call depth limit", expression->offset);
            calleeDepth = std::max(calleeDepth, callee.cost.depth);
            if (!asynchronous)
                calleeStackWords = std::max(calleeStackWords, callee.cost.stackWords);
        };
        const ResolveCall resolveCall = [&](const Expr& expression) {
            const auto callee = resolve(owner, mode, expression, receiverMode);
            account(expression, callee, expression->resolution == "static_async" ||
                    expression->resolution == "object_async" || expression->resolution == "local_async");
            return ResolvedCall{callee.symbol, callee.cost.unmeteredWork, callee.cost.retainsReceiver, callee.workerFields};
        };
        const ResolveObject resolveObject = [&](std::string_view name, std::size_t offset) -> const Declaration& {
            return scalarObject(owner, name, offset, mode);
        };
        const ResolveDispatch resolveDispatch = [&](const Expr& expression) {
            const auto& receiverType = expression->children.at(0)->children.at(0)->type;
            const bool throughInterface = layouts_.sourceInterface(receiverType);
            const bool asynchronous = expression->resolution == "source_interface_async" ||
                expression->resolution == "class_virtual_async";
            auto targetMode = mode == FunctionMode::Construction ? mode : FunctionMode::Contextual;
            if (asynchronous)
                targetMode = FunctionMode::Continuation;
            DispatchTargets targets;
            const auto bindTarget = [&](const Declaration& concrete, const std::string& identity) {
                const auto position = identity.find('.');
                const auto tag = layouts_.receiverTag(concrete);
                if (tag == 0)
                    failAtOwner(owner, "native dispatch requires a tagged source implementation", expression->offset);
                const auto& declaring = scalarObject(owner, std::string_view(identity).substr(0, position),
                                                     expression->offset, targetMode);
                const auto callee = compileCall(owner, declaring, identity, targetMode, false, false, expression->offset);
                account(expression, callee, asynchronous);
                targets.emplace_back(tag, callee.symbol);
            };
            for (const auto& concrete : declarations_) {
                if (concrete.kind != "Class")
                    continue;
                const bool compatible = throughInterface
                    ? std::find(concrete.interfaces.begin(), concrete.interfaces.end(), receiverType) != concrete.interfaces.end()
                    : concrete.name == receiverType.name ||
                      std::find(concrete.ancestors.begin(), concrete.ancestors.end(), receiverType.name) !=
                          concrete.ancestors.end();
                if (!compatible)
                    continue;

                // Retain the most derived target from the frontend's
                // checked implementation set for this concrete receiver.
                const auto* declaring = &concrete;
                while (declaring) {
                    const auto match = std::find_if(expression->targets.begin(), expression->targets.end(),
                                                   [&](const auto& identity) {
                        return std::string_view(identity).substr(0, identity.find('.')) == declaring->name;
                    });
                    if (match != expression->targets.end()) {
                        bindTarget(concrete, *match);
                        break;
                    }
                    declaring = declaring->baseClass.empty() ? nullptr :
                        layouts_.declaration(Type{declaring->baseClass, {}});
                }
            }
            return targets;
        };
        const ResolveParallel resolveParallel = [&](const Statement& statement) {
            Method worker;
            worker.name = "parallel." + std::to_string(nextSymbol_++);
            worker.identity = owner.name + "." + worker.name;
            worker.result = voidType;
            worker.offset = statement.offset;
            worker.staticMethod = true;
            worker.parameters.push_back({statement.type, statement.name, true, statement.offset});
            worker.parameters.insert(worker.parameters.end(), statement.parallelCaptures.begin(),
                                     statement.parallelCaptures.end());
            worker.body = statement.body;

            const auto workerName = owner.sourceName + ".$" + worker.name;
            auto fields = workerFields(owner, worker.body);
            appendWorkerParameters(worker, fields);
            symbols_.insert(workerName);
            emit(owner, worker, workerName, workerName, FunctionMode::Contextual, false, ReceiverMode::ParallelWorker);
            return ParallelWorker{workerName, std::move(fields)};
        };
        const bool hostedEntry = exported && hostedOwner_ && mode == FunctionMode::Continuation;
        auto binary32Checks = Binary32Checks::None;
        if (usesPortableBinary32_ && usesInvocationRuntime(mode))
            binary32Checks = Binary32Checks::HostValue;
        if (usesPortableBinary32_ && (hostedBindingConstructor ||
            (hostedEntry && !NativeNumericExports::admitsObservation(definition_, owner, method))))
            binary32Checks = Binary32Checks::EntryAndHostValue;
        auto lowering = ScalarLowering(owner, method, name, symbol, mode, exported, hostedEntry,
                                       resolveCall, resolveObject, resolveDispatch, resolveParallel, layouts_, receiverMode,
                                       hostedEntry && hostedObjectReferences_, hostedBindingConstructor,
                                       preserveSynchronousResult, !definition_.objectEnabled, binary32Checks);
        const auto retainsReceiver = lowering.retainsReceiver();
        if (hostedOwner_ && lowering.assignsReceiverReference())
            hostedGraph_ = true;
        auto function = lowering.finish();
        foldLiteralConcatenation(function);
        foldLiteralIntegerOperations(function);
        // The emitted graph counts each specialization once. Runtime-metered
        // bodies charge each execution through the shared segment budget.
        std::size_t emitted = 1 + function.slots.size() + function.blocks.size();
        std::size_t bodyWork = 1 + function.parameterCount + function.blocks.size();
        for (const auto& block : function.blocks) {
            emitted += block.instructions.size();
            bodyWork += block.instructions.size();
            for (const auto& instruction : block.instructions) {
                if (const auto* fill = std::get_if<FillFixedArray>(&instruction.operation))
                    bodyWork += valueWords(fill->array, layouts_.view());
            }
            if (block.exit) {
                if (const auto* dispatch = std::get_if<DispatchReceiver>(&*block.exit))
                    emitted += dispatch->branches.size();
                else if (const auto* cleanup = std::get_if<DispatchTransfer>(&*block.exit))
                    emitted += cleanup->destinations.size();
            }
        }
        if (emitted > maximumLoweredOperations - loweredOperations_)
            failAtOwner(owner, "native emitted call graph exceeds the lowering storage budget", method.offset);
        loweredOperations_ += emitted;

        if (!metered) {
            if ((!exported || unmeteredWork != 0) && bodyWork > (maximumUnmeteredWork - unmeteredWork) / 4)
                failAtOwner(owner, "native unmetered calls exceed the bounded evaluation cost", method.offset);
            unmeteredWork += bodyWork * 4;
        }

        auto stackWords = slotStorageWords(function, layouts_.view()) + function.receiverStorageWords;
        const auto scratch = runtimeScratch(function, layouts_.view());
        if (mode == FunctionMode::Construction)
            stackWords += function.childResultWords + scratch.hostArguments * 2 +
                scratch.hostArgumentWords + scratch.hostResultWords + scratch.arrayElementWords +
                (scratch.arrayElementWords ? 2 : 0) +
                (scratch.listElementWords ? 2 + scratch.listElementWords : 0) +
                scratch.sequenceValueWords + (scratch.sequenceValueWords ? 3 : 0) +
                scratch.dictionaryKeyWords + scratch.dictionaryValueWords + (scratch.dictionaryCalls ? 2 : 0) +
                (scratch.capturesError ? 2 : 0) + 1;
        stackWords += scratch.fixedArrayWords + scratch.resultInterfaceInputWords +
            scratch.resultInterfaceOutputWords + function.parallelCaptureWords + function.parallelCancellationSlots +
            function.concurrentSequenceWords + function.concurrentDictionaryWords +
            2 * scratch.comparisonWords + (scratch.textCalls ? 6 : 0) + (scratch.byteCalls ? 9 : 0);
        if (function.usesRuntime()) {
            stackWords += function.childResultWords + scratch.listElementWords + scratch.recordFieldWords +
                scratch.arrayElementWords + (scratch.arrayElementWords ? 2 : 0) +
                scratch.sequenceValueWords + (scratch.sequenceValueWords ? 3 : 0) +
                scratch.dictionaryKeyWords + scratch.dictionaryValueWords + (scratch.dictionaryCalls ? 2 : 0) +
                scratch.hostArguments * sizeof(native_app_v19::HostArgument) / sizeof(std::uint64_t) +
                scratch.hostArgumentWords + scratch.hostResultWords + (scratch.byteFieldWrites ? 3 : 0) + 8;
        }
        const auto ownStackWords = stackWords;
        stackWords += calleeStackWords;
        if (stackWords > maximumExplicitStackWords)
            failAtOwner(owner, "native synchronous call chain exceeds the stack storage limit", method.offset);

        active_.erase(active);
        ownStackWords_.push_back(ownStackWords);
        functions_.push_back(std::move(function));
        return {unmeteredWork, stackWords, calleeDepth + 1, retainsReceiver};
    }

    void validateRecursiveStack() const
    {
        std::map<std::string_view, std::size_t> indices;
        for (std::size_t index = 0; index < functions_.size(); ++index)
            indices.emplace(functions_[index].symbol, index);

        std::vector<std::vector<std::size_t>> callees(functions_.size());
        for (std::size_t index = 0; index < functions_.size(); ++index) {
            for (const auto& block : functions_[index].blocks) {
                for (const auto& instruction : block.instructions) {
                    if (const auto* call = std::get_if<DirectCall>(&instruction.operation))
                        callees[index].push_back(indices.at(call->destination));
                }
            }
        }

        // Each synchronous edge consumes one source frame. Async edges keep
        // their continuation state in the SDK and never add to this stack.
        auto bounds = ownStackWords_;
        auto next = bounds;
        for (std::size_t depth = 1; depth < model::SourceCallContext::maximumDepth; ++depth) {
            for (std::size_t index = 0; index < functions_.size(); ++index) {
                std::size_t childWords = 0;
                for (const auto callee : callees[index])
                    childWords = std::max(childWords, bounds[callee]);
                if (childWords > maximumExplicitStackWords - ownStackWords_[index]) {
                    const auto& function = functions_[index];
                    Diagnostic diagnostic("native recursive call chain exceeds the stack storage limit", function.offset);
                    diagnostic.path = function.sourceFile;
                    throw diagnostic;
                }
                next[index] = ownStackWords_[index] + childWords;
            }
            bounds.swap(next);
        }
    }

    const Program& definition_;
    const std::vector<Declaration>& declarations_;
    const Declaration* hostedOwner_ = nullptr;
    bool runtimeProgram_ = false;
    mutable bool hostedGraph_ = false;
    bool hostedObjectReferences_ = false;
    bool usesPortableBinary32_ = false;
    bool hasInvocationRecursion_ = false;
    bool hasHostedRecursion_ = false;
    using MethodVariant = std::pair<std::string, ReceiverMode>;

    std::multiset<MethodVariant> active_;
    std::set<std::string> symbols_;
    std::map<std::pair<MethodVariant, FunctionMode>, CompiledCall> compiled_;
    std::vector<ScalarFunction> functions_;
    std::vector<std::size_t> ownStackWords_;
    ValueLayouts layouts_;
    std::size_t nextSymbol_ = 0;
    std::size_t loweredOperations_ = 0;
};

}

ScalarProgram lowerScalarProgram(const CheckedProgram& checked, std::string_view entry)
{
    const auto [owner, declaring, selected, name] = selectEntry(checked, entry);
    if (!selected.pure || selected.asynchronous)
        failAtOwner(declaring, "native scalar entry must be a public static pure synchronous method", selected.offset);

    return ClosedCalls(checked).lower(declaring, selected, owner.sourceName + '.' + name, FunctionMode::Scalar);
}

ScalarProgram lowerRuntimeProgram(const CheckedProgram& checked, std::string_view entry)
{
    const auto [owner, declaring, parent, name] = selectEntry(checked, entry);
    const auto& result = parent.asynchronous ? parent.result.arguments.at(0) : parent.result;
    const bool wrappedResult = result == integerResult || result == voidResult;
    const bool supportedResult = wrappedResult ||
        (!parent.asynchronous && (result == Type{"int", {}} || result == voidType));
    if (!supportedResult || !launcherParameters(parent))
        failAtOwner(declaring, "native runtime entry must take an optional leading int or Array<string> and up to two "
                               "CancellationToken parameters. The result must be int or void, optionally returned as "
                               "Result<T,string> or Task<Result<T,string>>",
                    parent.offset);

    return ClosedCalls(checked).lower(declaring, parent, owner.sourceName + '.' + name, FunctionMode::Continuation);
}

ScalarProgram lowerNumericProgram(const CheckedProgram& checked, std::string_view entry, std::string symbol)
{
    const auto selected = selectEntry(checked, entry, true);
    if (!NativeNumericExports::admits(checked.definition(), selected.declaring, selected.method)) {
        failAtOwner(selected.declaring, "entry is outside the checked native numeric profile", selected.method.offset);
    }
    return ClosedCalls(checked).lowerNumeric(selected, std::move(symbol));
}

ScalarProgram lowerHostedProgram(const CheckedProgram& checked, std::string_view entry, std::string symbol,
                                 std::span<const std::string> methods, HostedOptions options)
{
    const auto primary = selectEntry(checked, entry, true);
    const auto& owner = primary.owner;
    const auto& method = primary.method;
    if (options.requiresExportedModel && !owner.modelExported)
        failAtOwner(owner, "native hosted entry class must be selected by --model-export", method.offset);
    if (methods.size() >= native_app_v19::maximumHostedMethods)
        failAtOwner(owner, "native hosted model exceeds the method count limit", method.offset);

    std::vector<SelectedMethod> additional;
    std::set<const Method*> selectedMethods{&method};
    for (const auto& selector : methods) {
        if (selector.substr(0, selector.find('(')).find('.') != std::string::npos)
            failAtOwner(owner, "native hosted methods must belong to the entry class", method.offset);
        auto selected = selectEntry(checked, owner.sourceName + '.' + selector, true);
        if (!selectedMethods.insert(&selected.method).second)
            failAtOwner(owner, "native hosted method was selected more than once", selected.method.offset);
        additional.push_back(std::move(selected));
    }
    const auto& constructor = selectHostedConstructor(owner, options.constructorSignature);
    return ClosedCalls(checked).lowerHosted(primary, constructor, std::move(symbol), additional, options);
}

std::size_t sourceLine(std::string_view source, std::size_t offset)
{
    return 1 + std::count(source.begin(), source.begin() + std::min(offset, source.size()), '\n');
}

}
