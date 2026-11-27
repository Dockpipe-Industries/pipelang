// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../CheckedProgram.hpp"
#include "ValueLayout.hpp"
#include "native_app_abi.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace pipelang::models { struct CompositionGraph; }

namespace pipelang::models::native_ir {

struct Target;

enum class Predicate { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
enum class CollectionOperation { At, Count };
enum class SetOperation { Contains, Add, Remove, ToList };

inline bool indirectArgument(NativeType type)
{
    return usesAggregateLayout(type) || type == ScalarType::Text || type == ScalarType::Error ||
        type == ScalarType::ByteBuffer || type == ScalarType::ClassReference;
}

struct Register {
    std::size_t index;
};

struct ZeroValue {};

struct Operand {
    NativeType type;
    std::variant<Register, bool, std::int64_t, ZeroValue> value;
};

struct Slot {
    std::string name;
    NativeType type;
    std::size_t offset;
};

struct Load {
    Register result;
    std::size_t slot;
};

struct Store {
    std::size_t slot;
    Operand value;
    bool declaration;
};

struct Compare {
    Register result;
    Predicate predicate;
    Operand left;
    Operand right;
};

struct CompareOwned {
    Register result;
    Operand left;
    Operand right;
    bool negate;
};

enum class CallAbi { Scalar, Contextual, Construction };

struct DirectCall {
    std::optional<Register> result;
    std::string destination;
    std::vector<Operand> arguments;
    CallAbi abi;
    std::optional<NativeType> resultType = {};
    bool closesConstruction = false;
};

struct HostCall {
    std::optional<Register> immediateResult;
    Operand service;
    std::string interfaceName;
    std::string signature;
    std::vector<Operand> arguments;
    NativeType resultType;
    std::optional<NativeType> response;
    bool wrapsHostStatus;
    std::optional<std::size_t> reusableByteSlot = {};
};

struct AllocateObject {
    Register result;
    std::size_t frameOffset;
    std::size_t words;
    std::size_t tag;
    bool arena;
    bool graphEligible;
};

struct ReadReceiverTag {
    Register result;
    Operand receiver;
};

struct RequireReceiver {
    Operand receiver;
};

struct FillFixedArray {
    Register result;
    NativeType array;
    Operand initial;
    bool validateOwnership;
};

struct FixedArrayIndex {
    Register result;
    Operand array;
    Operand index;
    NativeType element;
    std::optional<Operand> replacement;
    bool validateOwnership;
};

struct ReadField {
    Register result;
    Operand receiver;
    std::size_t field;
    NativeType type;
};

struct WriteField {
    Operand receiver;
    std::size_t field;
    Operand value;
    bool initializing = false;
};

enum class ResultKind { Success, Failure, Cancelled };
enum class ResultProperty { IsSuccess, IsFailure, IsCancelled, Value, SuccessPayload, Error };

struct TextLiteral {
    Register result;
    std::string bytes;
};

enum class TextOperation {
    Concatenate, ByteLengthIs, ByteAtOr, StartsWith, FromInt, Utf8Length, Utf8Slice, Utf8IndexOf
};

struct TextCall {
    Register result;
    TextOperation operation;
    std::vector<Operand> arguments;
    NativeType resultType;
};

struct ByteLength {
    Register result;
    Operand receiver;
};

struct ByteCall {
    Register result;
    native_app_v19::ByteOperation operation;
    NativeType resultType;
    std::vector<Operand> arguments;
};

struct ConstructResult {
    Register result;
    NativeType type;
    ResultKind kind;
    std::optional<Operand> value;
    Operand error;
};

struct EraseResult {
    Register result;
    Operand source;
    NativeType erased;
};

struct ValidateConstructedValue {
    Operand value;
};

struct ReadResult {
    Register result;
    Operand receiver;
    ResultProperty property;
};

struct ConstructOptional {
    Register result;
    NativeType type;
    std::optional<Operand> value;
};

struct ReadOptional {
    Register result;
    Operand receiver;
    bool projectValue;
};

struct ConstructRecord {
    Register result;
    NativeType type;
    std::vector<Operand> fields;
};

struct ReadRecordField {
    Register result;
    Operand record;
    std::size_t field;
    NativeType type;
};

struct UpdateRecordField {
    Register result;
    Operand record;
    std::vector<std::size_t> path;
    Operand value;
};

struct CollectionCall {
    CollectionOperation operation;
    Register result;
    Operand receiver;
    std::optional<Operand> index;
};

struct ArrayCall {
    native_app_v19::ArrayOperation operation;
    Register result;
    NativeType array;
    std::optional<Operand> receiver;
    Operand index;
    std::optional<Operand> value;
};

struct ListAppend {
    Register result;
    Operand list;
    Operand element;
};

struct SetCall {
    SetOperation operation;
    Register result;
    Operand set;
    std::optional<Operand> key;
    std::optional<Register> updated;
    NativeType resultType;
};

struct SequenceCall {
    native_app_v19::SequenceOperation operation;
    Operand sequence;
    std::optional<Operand> argument;
    std::optional<Operand> index;
    std::optional<Register> updated;
    std::optional<Operand> result;
};

struct DictionaryCall {
    native_app_v19::DictionaryOperation operation;
    Operand dictionary;
    std::optional<Operand> key;
    std::optional<Operand> value;
    std::optional<NativeType> projection;
    std::optional<Operand> index;
    std::optional<Register> updated;
    std::optional<Operand> result;
};

enum class IntegerOperation { Add, Subtract, Multiply, Divide, Remainder };

struct CheckedInteger {
    Register result;
    IntegerOperation operation;
    Operand left;
    Operand right;
    std::optional<std::int64_t> knownValue = std::nullopt;
};

enum class UInt32Operation {
    Add, Subtract, Multiply, Divide, Remainder, ShiftLeft, ShiftRight, FromInt,
    And, Or, Xor, WrappingAdd, WrappingSubtract, WrappingMultiply
};

struct UInt32Compute {
    Register result;
    UInt32Operation operation;
    Operand left;
    Operand right;
};

enum class FloatingArithmetic { Add, Subtract, Multiply, Divide, Negate };

struct FloatingCompute {
    Register result;
    FloatingArithmetic operation;
    Operand left;
    std::optional<Operand> right;
};

struct FloatingCall {
    Register result;
    native_app_v19::FloatingBuiltin operation;
    NativeType floatingType;
    NativeType resultType;
    std::vector<Operand> arguments;
};

struct BudgetStep {
    std::size_t count = 1;
};
struct BeginCleanup {};
struct EndCleanup {};
struct CaptureError {
    Register result;
};
struct RaiseError {
    Operand error;
};
struct CancellationRequested {
    Register result;
    Operand token;
};

struct CreateContextOwner {
    Register result;
};

struct ContextOwnerCall {
    Operand receiver;
    native_app_v19::ContextOwnerOperation operation;
};

struct CreateSemaphore {
    std::size_t site;
    Operand initial;
    std::optional<Operand> maximum;
};

struct CreateSharedSemaphore {
    Register result;
    Operand initial;
    std::optional<Operand> maximum;
};

struct ShareSemaphore {
    Register result;
    Operand semaphore;
};

struct ReadSemaphoreCount {
    Register result;
    Operand semaphore;
};

struct ReleaseSemaphore {
    Register result;
    Operand semaphore;
    Operand count;
};

struct CreateConcurrentDictionary {
    Register result;
    NativeType type;
};

struct ConcurrentDictionaryCall {
    native_app_v19::ConcurrentDictionaryOperation operation;
    Operand receiver;
    std::vector<Operand> inputs;
    std::optional<Operand> output;
};

struct CreateConcurrentSequence {
    Register result;
    NativeType type;
};

struct ConcurrentSequenceCall {
    native_app_v19::ConcurrentSequenceOperation operation;
    Operand receiver;
    std::optional<Operand> input;
    std::optional<Operand> output;
};

struct ParallelInvocation {
    Operand sequence;
    NativeType item;
    std::string worker;
    std::vector<Operand> captures;
    std::vector<Operand> cancellation;
};

struct WaitSemaphore {
    std::optional<Register> result;
    Operand semaphore;
    Operand timeout;
    std::optional<Operand> cancellation;
};

struct CreateLock {
    Register result;
};

struct AcquireLock {
    Operand lock;
};

struct ReleaseLock {};

struct CreateAtomic {
    Register result;
    native_app_v19::AtomicValueKind kind;
    Operand initial;
    std::optional<std::size_t> localSite = {};
};

inline native_app_v19::AtomicValueKind atomicKind(NativeType type)
{
    using Kind = native_app_v19::AtomicValueKind;
    switch (type.kind) {
    case ScalarType::Integer: return Kind::Integer;
    case ScalarType::Boolean: return Kind::Boolean;
    case ScalarType::Float: return Kind::Float;
    case ScalarType::Float32: return Kind::Float32;
    default: throw std::logic_error("unrecognized native atomic value type");
    }
}

struct AtomicCall {
    std::optional<Register> result;
    NativeType valueType;
    native_app_v19::AtomicValueKind kind;
    native_app_v19::AtomicOperation operation;
    Operand receiver;
    std::vector<Operand> arguments;
};

struct ResumedWait {
    Register result;
};

struct ResumedCall {
    Register result;
    NativeType type;
};

struct Suspend {
    Operand semaphore;
    Operand timeout;
    std::optional<Operand> cancellation;
    std::size_t resume;
};

struct AwaitCall {
    std::string destination;
    std::optional<Operand> receiver;
    std::vector<Operand> arguments;
    std::size_t resume;
    std::vector<std::optional<std::size_t>> argumentSlots;
};

struct AwaitHost {
    std::size_t resume;
};

struct Jump {
    std::size_t destination;
};

struct Branch {
    Operand condition;
    std::size_t whenTrue;
    std::size_t whenFalse;
};

struct Return {
    std::optional<Operand> value;
};

struct DispatchTransfer {
    Operand selector;
    std::vector<std::size_t> destinations;
};

struct DispatchReceiver {
    Operand tag;
    std::vector<std::pair<std::size_t, std::size_t>> branches;
    std::size_t fallback;
};

struct InvalidDispatch {};
struct Unreachable {};

using SynchronizationOperation = std::variant<
    CreateContextOwner, ContextOwnerCall, CreateSemaphore, CreateSharedSemaphore, ShareSemaphore,
    ReadSemaphoreCount, ReleaseSemaphore,
    CreateConcurrentSequence, ConcurrentSequenceCall, CreateConcurrentDictionary, ConcurrentDictionaryCall, ParallelInvocation, WaitSemaphore, CreateLock, AcquireLock, ReleaseLock, CreateAtomic, AtomicCall>;

struct Instruction {
    using Operation = std::variant<Load, Store, Compare, DirectCall, HostCall, AllocateObject, ReadField, WriteField, TextLiteral, TextCall,
                 ConstructResult, ReadResult, EraseResult, ValidateConstructedValue, CollectionCall, ArrayCall,
                 CheckedInteger, UInt32Compute,
                 FloatingCompute, FloatingCall, ByteLength, ByteCall, BudgetStep,
                 SynchronizationOperation,
                 ResumedWait, ResumedCall,
                 BeginCleanup, EndCleanup, CaptureError, RaiseError, CancellationRequested,
                 ConstructRecord, ReadRecordField, UpdateRecordField, ListAppend, SetCall, SequenceCall, ConstructOptional,
                 ReadOptional, CompareOwned, DictionaryCall,
                 ReadReceiverTag, RequireReceiver, FillFixedArray, FixedArrayIndex>;

    Instruction(std::size_t offset, Operation operation)
        : offset(offset), operation(std::move(operation))
    {
    }

    template<class Sync> requires std::is_constructible_v<SynchronizationOperation, Sync>
    Instruction(std::size_t offset, Sync operation)
        : offset(offset), operation(SynchronizationOperation{std::move(operation)})
    {
    }

    std::size_t offset;
    Operation operation;
};

struct Block {
    std::vector<Instruction> instructions;
    std::size_t exitOffset = 0;
    std::optional<std::variant<Jump, Branch, Return, Unreachable, Suspend, AwaitCall, AwaitHost, DispatchTransfer,
                               DispatchReceiver, InvalidDispatch>> exit;
    std::size_t faultTarget = 0;
};

enum class FunctionMode { Scalar, Contextual, Continuation, Construction, InvocationConstruction };

inline bool usesInvocationRuntime(FunctionMode mode)
{
    return mode == FunctionMode::Contextual || mode == FunctionMode::Continuation ||
        mode == FunctionMode::InvocationConstruction;
}

struct ScalarFunction {
    std::string name;
    std::string symbol;
    std::string sourceFile;
    std::size_t offset;
    std::size_t parameterCount;
    FunctionMode mode;
    bool exported;
    bool returnsVoid;
    std::vector<Slot> slots;
    std::vector<Block> blocks;
    std::size_t parallelCaptureWords = 0;
    std::size_t parallelCancellationSlots = 0;
    std::size_t concurrentSequenceWords = 0;
    std::size_t concurrentDictionaryWords = 0;
    std::size_t receiverStorageWords = 0;
    NativeType result = ScalarType::Integer;
    std::size_t childResultWords = 0;
    bool checksInvocationCancellation = false;
    bool checksPortableBinary32OnPoll = false;

    bool usesRuntime() const { return usesInvocationRuntime(mode); }
    bool continuation() const { return mode == FunctionMode::Continuation; }
    bool construction() const { return mode == FunctionMode::Construction || mode == FunctionMode::InvocationConstruction; }
};

struct HostedParameter {
    NativeType type;
    std::string service;
};

struct HostedRecipeEntry {
    std::string service;
    std::string callable;
    bool factory;
    std::optional<std::size_t> cancellationIndex;
    NativeType receiverType;
    std::vector<HostedParameter> parameters;
    std::size_t receiverWords;
    std::size_t receiverTag;
    std::string cleanup;
};

struct HostedMethodEntry {
    std::string name;
    std::size_t function;
};

struct HostedCompositionVariant {
    std::size_t receiverTag;
    std::size_t receiverWords;
    std::vector<HostedMethodEntry> methods;
    std::string cleanup;
};

struct HostedCompositionService {
    std::string name;
    std::uint64_t lifetime;
    std::uint64_t input;
    std::uint64_t flags;
    std::size_t recipe;
    std::vector<std::size_t> dependencies;
    std::optional<NativeType> snapshot;
    std::vector<HostedMethodEntry> methods;
    std::vector<HostedCompositionVariant> variants;
};

struct HostedSnapshotField {
    std::string name;
    NativeType type;
    std::size_t offset;
};

struct HostedSnapshot {
    std::string type;
    std::vector<HostedSnapshotField> fields;
};

struct HostedEntry {
    std::string symbol;
    std::string constructor;
    std::vector<HostedParameter> parameters;
    std::size_t receiverWords;
    std::size_t receiverTag;
    bool retainsObjectGraphs;
    std::vector<HostedMethodEntry> methods;
    std::optional<HostedSnapshot> snapshot;
    bool allowsMultipleTasks = false;
    bool exposesObjectReferences = false;
    std::vector<HostedRecipeEntry> recipes = {};
    std::vector<HostedCompositionService> composition = {};
};

struct HostedOptions {
    bool exposesSnapshot = false;
    bool allowsMultipleTasks = false;
    bool exposesObjectReferences = false;
    std::string_view constructorSignature;
    const CompositionGraph* composition = nullptr;
    bool activeHostFloating = false;
    bool requiresExportedModel = false;
};

struct ScalarProgram {
    std::vector<ScalarFunction> functions;
    std::vector<ValueLayout> layouts;
    std::optional<HostedEntry> hosted = {};
    bool tracksSourceCallDepth = false;
    bool tracksHostedSourceCallDepth = false;
    std::optional<std::string> numericExport = {};
};

inline std::size_t slotStorageWords(const ScalarFunction& function, std::span<const ValueLayout> layouts)
{
    std::size_t words = 0;
    for (const auto& slot : function.slots)
        words += valueWords(slot.type, layouts);
    return words;
}

struct RuntimeScratch {
    std::size_t arrayElementWords = 0;
    std::size_t fixedArrayWords = 0;
    std::size_t listElementWords = 0;
    std::size_t comparisonWords = 0;
    std::size_t resultInterfaceInputWords = 0;
    std::size_t resultInterfaceOutputWords = 0;
    std::size_t hostArguments = 0;
    std::size_t hostArgumentWords = 0;
    std::size_t hostResultWords = 0;
    std::size_t recordFieldWords = 0;
    std::size_t sequenceValueWords = 0;
    std::size_t dictionaryKeyWords = 0;
    std::size_t dictionaryValueWords = 0;
    bool dictionaryCalls = false;
    bool capturesError = false;
    bool textCalls = false;
    bool byteCalls = false;
    bool byteFieldWrites = false;
};

inline RuntimeScratch runtimeScratch(const ScalarFunction& function, std::span<const ValueLayout> layouts)
{
    RuntimeScratch scratch;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (const auto* fill = std::get_if<FillFixedArray>(&instruction.operation)) {
                scratch.fixedArrayWords = std::max(scratch.fixedArrayWords, valueWords(fill->array, layouts));
            } else if (const auto* index = std::get_if<FixedArrayIndex>(&instruction.operation)) {
                scratch.fixedArrayWords = std::max(scratch.fixedArrayWords, valueWords(index->array.type, layouts));
            } else if (const auto* append = std::get_if<ListAppend>(&instruction.operation)) {
                scratch.listElementWords = std::max(scratch.listElementWords, valueWords(append->element.type, layouts));
            } else if (const auto* set = std::get_if<SetCall>(&instruction.operation)) {
                const auto key = layouts[set->set.type.layout].fields.front().type;
                scratch.listElementWords = std::max(scratch.listElementWords, valueWords(key, layouts));
            } else if (const auto* sequence = std::get_if<SequenceCall>(&instruction.operation)) {
                if (sequence->argument)
                    scratch.sequenceValueWords = std::max(scratch.sequenceValueWords,
                        valueWords(sequence->argument->type, layouts));
                if (sequence->result)
                    scratch.sequenceValueWords = std::max(scratch.sequenceValueWords,
                        valueWords(sequence->result->type, layouts));
            } else if (const auto* dictionary = std::get_if<DictionaryCall>(&instruction.operation)) {
                scratch.dictionaryCalls = true;
                if (dictionary->key)
                    scratch.dictionaryKeyWords = std::max(scratch.dictionaryKeyWords,
                        valueWords(dictionary->key->type, layouts));
                if (dictionary->value)
                    scratch.dictionaryValueWords = std::max(scratch.dictionaryValueWords,
                        valueWords(dictionary->value->type, layouts));
                if (dictionary->result)
                    scratch.dictionaryValueWords = std::max(scratch.dictionaryValueWords,
                        valueWords(dictionary->result->type, layouts));
            } else if (const auto* array = std::get_if<ArrayCall>(&instruction.operation)) {
                const auto element = layouts[array->array.layout].fields.front().type;
                scratch.arrayElementWords = std::max(scratch.arrayElementWords, valueWords(element, layouts));
            } else if (const auto* call = std::get_if<CollectionCall>(&instruction.operation)) {
                if (call->receiver.type == ScalarType::List && call->operation == CollectionOperation::At) {
                    const auto element = layouts[call->receiver.type.layout].fields.front().type;
                    scratch.listElementWords = std::max(scratch.listElementWords, valueWords(element, layouts));
                }
            } else if (const auto* erased = std::get_if<EraseResult>(&instruction.operation)) {
                scratch.resultInterfaceInputWords = std::max(scratch.resultInterfaceInputWords,
                    valueWords(erased->source.type, layouts));
                scratch.resultInterfaceOutputWords = std::max(scratch.resultInterfaceOutputWords,
                    valueWords(erased->erased, layouts));
            } else if (const auto* check = std::get_if<ValidateConstructedValue>(&instruction.operation)) {
                scratch.resultInterfaceInputWords = std::max(scratch.resultInterfaceInputWords,
                    valueWords(check->value.type, layouts));
            } else if (const auto* compare = std::get_if<CompareOwned>(&instruction.operation)) {
                scratch.comparisonWords = std::max(scratch.comparisonWords, valueWords(compare->left.type, layouts));
            } else if (const auto* call = std::get_if<HostCall>(&instruction.operation)) {
                scratch.hostArguments = std::max(scratch.hostArguments, call->arguments.size());
                std::size_t words = 0;
                for (const auto& argument : call->arguments)
                    words += valueWords(argument.type, layouts);
                scratch.hostArgumentWords = std::max(scratch.hostArgumentWords, words);
                if (call->immediateResult)
                    scratch.hostResultWords = std::max(scratch.hostResultWords, valueWords(call->resultType, layouts));
            } else if (std::holds_alternative<CaptureError>(instruction.operation)) {
                scratch.capturesError = true;
            } else if (std::holds_alternative<TextCall>(instruction.operation)) {
                scratch.textCalls = true;
            } else if (std::holds_alternative<ByteCall>(instruction.operation)) {
                scratch.byteCalls = true;
            } else if (const auto* write = std::get_if<WriteField>(&instruction.operation)) {
                scratch.byteFieldWrites |= write->value.type == ScalarType::ByteBuffer;
                if (write->value.type == ScalarType::Record || write->value.type == ScalarType::HashSet ||
                    containsReceiverValues(write->value.type, layouts))
                    scratch.recordFieldWords = std::max(scratch.recordFieldWords, valueWords(write->value.type, layouts));
            } else if (const auto* read = std::get_if<ReadField>(&instruction.operation)) {
                if (containsReceiverSets(read->type, layouts) || containsReceiverValues(read->type, layouts))
                    scratch.recordFieldWords = std::max(scratch.recordFieldWords, valueWords(read->type, layouts));
            }
        }
    }
    return scratch;
}

ScalarProgram lowerScalarProgram(const CheckedProgram& checked, std::string_view entry);
ScalarProgram lowerRuntimeProgram(const CheckedProgram& checked, std::string_view entry);
ScalarProgram lowerNumericProgram(const CheckedProgram& checked, std::string_view entry, std::string symbol);
ScalarProgram lowerHostedProgram(const CheckedProgram& checked, std::string_view entry, std::string symbol,
                                std::span<const std::string> methods = {}, HostedOptions options = {});
std::size_t sourceLine(std::string_view source, std::size_t offset);
std::string emitLlvmIr(const ScalarProgram& program, const std::vector<ModuleSource>& sources,
                       const Target& target, bool debug, bool sourceTrace, bool activeHostFloating);

}
