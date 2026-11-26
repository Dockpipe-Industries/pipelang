// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ScalarFunction.hpp"
#include "Target.hpp"
#include "native_app_abi.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <sstream>

namespace pipelang::models::native_ir {
namespace {

std::optional<native_app_v19::ReceiverSynchronizationKind> receiverSynchronizationKind(NativeType type)
{
    using Kind = native_app_v19::ReceiverSynchronizationKind;
    switch (type.kind) {
    case ScalarType::AtomicReference: return Kind::Atomic;
    case ScalarType::LockReference: return Kind::Lock;
    case ScalarType::Semaphore: return Kind::Semaphore;
    case ScalarType::ConcurrentSequenceReference: return Kind::ConcurrentSequence;
    case ScalarType::ConcurrentDictionaryReference: return Kind::ConcurrentDictionary;
    case ScalarType::ContextOwnerReference: return Kind::ContextOwner;
    default: return {};
    }
}

bool takesProcessArguments(const ScalarFunction& function)
{
    return function.exported && function.continuation() && function.parameterCount != 0 &&
        function.slots.front().type == ScalarType::Array;
}

std::string llvmType(NativeType type)
{
    if (type == ScalarType::Boolean)
        return "i1";
    if (type == ScalarType::Text || type == ScalarType::Error)
        return "%pipe.text";
    if (type == ScalarType::ByteBuffer)
        return "%pipe.bytes";
    if (usesAggregateLayout(type))
        return "%pipe.value." + std::to_string(type.layout);
    if (type == ScalarType::Receiver || type == ScalarType::Interface || type == ScalarType::ClassReference ||
        type == ScalarType::CancellationToken)
        return "ptr";

    return "i64";
}

std::string storageType(NativeType type)
{
    if (type == ScalarType::Boolean)
        return "i8";

    return llvmType(type);
}

std::string fieldStorageType(NativeType type)
{
    // Every aggregate field starts on a word boundary, including booleans.
    return type == ScalarType::Boolean ? "i64" : llvmType(type);
}

std::size_t debugType(NativeType type, std::span<const std::size_t> layoutMetadata)
{
    switch (type.kind) {
    case ScalarType::Integer: return 2;
    case ScalarType::UInt32: return 23;
    case ScalarType::Float: return 24;
    case ScalarType::Float32: return 25;
    case ScalarType::ByteBuffer: return 26;
    case ScalarType::Boolean: return 3;
    case ScalarType::Receiver:
    case ScalarType::Interface: return 7;
    case ScalarType::Text: return 8;
    case ScalarType::Error: return 18;
    case ScalarType::Enum:
    case ScalarType::Optional:
    case ScalarType::Result:
    case ScalarType::ResultInterface:
    case ScalarType::List:
    case ScalarType::Array:
    case ScalarType::HashSet:
    case ScalarType::Queue:
    case ScalarType::Stack:
    case ScalarType::Dictionary:
    case ScalarType::Pair:
    case ScalarType::ClassReference:
    case ScalarType::FixedArray:
    case ScalarType::Record: return layoutMetadata[type.layout];
    case ScalarType::AtomicReference:
    case ScalarType::ConcurrentSequenceReference:
    case ScalarType::ConcurrentDictionaryReference:
    case ScalarType::LockReference:
    case ScalarType::Semaphore:
    case ScalarType::Service: return 2;
    case ScalarType::CancellationToken: return 22;
    case ScalarType::ContextOwnerReference: return 2;
    }
    throw std::logic_error("unrecognized native debug type");
}

const char* llvmPredicate(Predicate predicate, bool unsignedOrder)
{
    switch (predicate) {
    case Predicate::Equal: return "eq";
    case Predicate::NotEqual: return "ne";
    case Predicate::Less: return unsignedOrder ? "ult" : "slt";
    case Predicate::LessEqual: return unsignedOrder ? "ule" : "sle";
    case Predicate::Greater: return unsignedOrder ? "ugt" : "sgt";
    case Predicate::GreaterEqual: return unsignedOrder ? "uge" : "sge";
    }

    throw std::logic_error("unrecognized native comparison predicate");
}

const char* llvmFloatingPredicate(Predicate predicate)
{
    switch (predicate) {
    case Predicate::Equal: return "oeq";
    case Predicate::NotEqual: return "une";
    case Predicate::Less: return "olt";
    case Predicate::LessEqual: return "ole";
    case Predicate::Greater: return "ogt";
    case Predicate::GreaterEqual: return "oge";
    }
    throw std::logic_error("unrecognized native floating comparison");
}

const char* floatingSuffix(NativeType type) { return type == ScalarType::Float32 ? "f32" : "f64"; }
const char* llvmFloatingType(NativeType type) { return type == ScalarType::Float32 ? "float" : "double"; }

std::string registerName(Register reg)
{
    return "%value" + std::to_string(reg.index);
}

std::string operandName(const Operand& operand)
{
    if (const auto* reg = std::get_if<Register>(&operand.value))
        return registerName(*reg);
    if (const auto* boolean = std::get_if<bool>(&operand.value))
        return *boolean ? "true" : "false";
    if (std::holds_alternative<ZeroValue>(operand.value))
        return "zeroinitializer";
    if (operand.type == ScalarType::CancellationToken)
        return "null";
    if (usesCollectionHandle(operand.type.kind))
        return "zeroinitializer";

    return std::to_string(std::get<std::int64_t>(operand.value));
}

std::string llvmString(std::string_view value)
{
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";

    std::string escaped;
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '"' || byte == '\\' || byte < 32 || byte >= 127) {
            escaped += '\\';
            escaped += hexadecimal[byte >> 4];
            escaped += hexadecimal[byte & 15];
        } else {
            escaped += raw;
        }
    }

    return escaped;
}

std::string asyncDescriptor(std::string_view function)
{
    return "@\".pipe.async." + llvmString(function) + '"';
}

std::string ownedShapeName(NativeType type)
{
    switch (type.kind) {
    case ScalarType::Integer: return "@.pipe.owned.int";
    case ScalarType::UInt32: return "@.pipe.owned.uint32";
    case ScalarType::Float: return "@.pipe.owned.float";
    case ScalarType::Float32: return "@.pipe.owned.float32";
    case ScalarType::ByteBuffer: return "@.pipe.owned.bytes";
    case ScalarType::Boolean: return "@.pipe.owned.bool";
    case ScalarType::Text: return "@.pipe.owned.text";
    case ScalarType::Error: return "@.pipe.owned.error";
    case ScalarType::Interface: return "@.pipe.owned.reference";
    case ScalarType::Semaphore: return "@.pipe.owned.semaphore";
    case ScalarType::LockReference: return "@.pipe.owned.lock";
    case ScalarType::ContextOwnerReference: return "@.pipe.owned.context";
    case ScalarType::ClassReference:
    case ScalarType::ConcurrentSequenceReference:
    case ScalarType::ConcurrentDictionaryReference:
    case ScalarType::AtomicReference:
    case ScalarType::FixedArray:
    case ScalarType::Array:
    case ScalarType::Record:
    case ScalarType::Result:
    case ScalarType::ResultInterface:
    case ScalarType::Enum:
    case ScalarType::Optional:
    case ScalarType::HashSet:
    case ScalarType::Queue:
    case ScalarType::Stack:
    case ScalarType::Dictionary:
    case ScalarType::Pair:
    case ScalarType::List: return "@.pipe.owned." + std::to_string(type.layout);
    default: throw std::logic_error("native value cannot be stored in an owned list");
    }
}

class LlvmIrEmitter final {
public:
    LlvmIrEmitter(const ScalarFunction& function, std::string_view source, const Target& target, bool debug,
                  std::size_t metadataStart, std::span<const ValueLayout> layouts,
                  std::span<const std::size_t> layoutMetadata, bool tracksSourceCallDepth, bool sourceTrace,
                  bool retainsObjectGraphs, bool allowsMultipleTasks, bool activeHostFloating, bool retainsScopedBytes)
        : function_(function), target_(target), debug_(debug), metadataStart_(metadataStart),
          layouts_(layouts), layoutMetadata_(layoutMetadata), tracksSourceCallDepth_(tracksSourceCallDepth),
          retainsObjectGraphs_(retainsObjectGraphs), allowsMultipleTasks_(allowsMultipleTasks),
          activeHostFloating_(activeHostFloating), retainsScopedBytes_(retainsScopedBytes)
    {
        if (sourceTrace && function_.usesRuntime()) {
            traceFunction_ = hostString("trace.function", function_.name, 0);
            traceFile_ = hostString("trace.file", function_.sourceFile, 0);
        }
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (source[index] == '\n')
                newlines_.push_back(index);
        }
        hasFaultTargets_ = std::any_of(function_.blocks.begin(), function_.blocks.end(), [](const Block& block) {
            return block.faultTarget != 0;
        });
        usesFloating_ = std::any_of(function_.blocks.begin(), function_.blocks.end(), [](const Block& block) {
            return std::any_of(block.instructions.begin(), block.instructions.end(), [](const Instruction& instruction) {
                const auto* compare = std::get_if<Compare>(&instruction.operation);
                return std::holds_alternative<FloatingCompute>(instruction.operation) ||
                    std::holds_alternative<FloatingCall>(instruction.operation) ||
                    (compare && isFloating(compare->left.type));
            });
        });
    }

    std::string emit()
    {
        output_ << "define ";
        if (!function_.exported)
            output_ << "internal ";
        const auto contextual = function_.mode == FunctionMode::Contextual;
        auto returnType = function_.result == ScalarType::Record ? llvmType(function_.result) : "i64";
        if (contextual || function_.construction())
            returnType = "i32";
        else if (function_.returnsVoid || function_.continuation())
            returnType = "void";
        output_ << returnType << " @\"" << llvmString(function_.symbol) << "\"(";
        if (function_.continuation()) {
            output_ << "ptr %runtime, ptr %receiver, ptr %arguments, i64 %acquired, ptr %storage, "
                       "i64 %programCounter, ptr %childResult, ptr %step, ptr %resultOutput";
        } else {
            if (contextual)
                output_ << "ptr %runtime, ptr %resultOutput";
            if (function_.construction()) {
                output_ << (function_.usesRuntime() ? "ptr %runtime, ptr %allocateReceiver"
                                                 : "ptr %arena, ptr %allocateReceiver");
                if (!function_.returnsVoid)
                    output_ << ", ptr %resultOutput";
            }
            for (std::size_t index = 0; index < function_.parameterCount; ++index) {
                if (contextual || function_.construction() || index != 0)
                    output_ << ", ";
                output_ << llvmType(function_.slots[index].type) << " %argument" << index;
            }
        }
        output_ << ')';
        if (usesFloating_)
            output_ << " strictfp";
        if (debug_ && contextual)
            output_ << " \"frame-pointer\"=\"all\"";
        output_ << " !dbg !" << subprogram() << " {\n";

        if (function_.usesRuntime())
            emitRuntimeEntry();
        for (std::size_t index = 0; index < function_.blocks.size(); ++index)
            emitBlock(index, function_.blocks[index]);
        if (function_.usesRuntime()) {
            for (std::size_t index = 0; index < sourceGuards_.size(); ++index) {
                const auto& guard = sourceGuards_[index];
                output_ << "source_guard_" << index << ":\n"
                        << "  call void @pipelang_native_v19_" << guard.diagnostic << "(ptr %runtime)";
                location(guard.offset);
                output_ << '\n';
                emitSourceNote("failure", guard.offset);
                output_ << "  br label %runtime_failure\n";
            }
            output_ << "invalid_result_value:\n  call void @pipelang_native_v19_invalid_result_value(ptr %runtime)\n"
                       "  br label %runtime_failure\n"
                       "invalid_optional_value:\n  call void @pipelang_native_v19_invalid_optional_value(ptr %runtime)\n"
                       "  br label %runtime_failure\n"
                       "invalid_fixed_array_index:\n  call void @pipelang_native_v19_invalid_fixed_array_index(ptr %runtime)\n"
                       "  br label %runtime_failure\n"
                       "uninitialized_receiver:\n  call void @pipelang_native_v19_uninitialized_receiver(ptr %runtime)\n"
                       "  br label %runtime_failure\n"
                       "uninitialized_context_owner:\n  call void @pipelang_native_v19_uninitialized_context_owner(ptr %runtime)\n"
                       "  br label %runtime_failure\nruntime_failure:\n";
            if (!function_.continuation()) {
                emitContextualFailure();
            } else {
                if (hasFaultTargets_)
                    saveDebugSlots("%fault");
                output_ << "  ret void\n";
            }
            if (hasFaultTargets_)
                output_ << "invalid_cleanup_transfer:\n  unreachable\n";
        }
        if (hasHostedResultGuard_)
            output_ << "invalid_result_value:\n"
                       "  %invalidResult = call i32 @pipelang_native_v19_hosted_invalid_result_value(ptr %arena)\n"
                       "  ret i32 %invalidResult\n";
        if (function_.construction() && !function_.usesRuntime()) {
            emitHostedGuards();
            output_ << "receiver_allocation_failed:\n  ret i32 1\nuninitialized_receiver:\n  ret i32 2\n"
                    << "hosted_construction_failed:\n  ret i32 3\nuninitialized_context_owner:\n  ret i32 4\n";
            if (hasFaultTargets_)
                output_ << "invalid_cleanup_transfer:\n  unreachable\n";
        }
        output_ << "}\n";
        output_ << textGlobals_.str();

        emitMetadata();

        return output_.str();
    }

    std::size_t nextMetadata() const
    {
        return slotMetadata(function_.slots.size()) + locations_.size();
    }

private:
    std::size_t line(std::size_t offset) const
    {
        return 1 + static_cast<std::size_t>(
            std::lower_bound(newlines_.begin(), newlines_.end(), offset) - newlines_.begin());
    }

    std::size_t slotMetadata(std::size_t slot) const
    {
        return metadataStart_ + 5 + slot;
    }

    std::size_t file() const { return metadataStart_ + 1; }
    std::size_t subprogram() const { return metadataStart_ + 2; }
    std::size_t signature() const { return metadataStart_ + 3; }
    std::size_t parameterTypes() const { return metadataStart_ + 4; }

    void location(std::size_t offset)
    {
        const auto sourceLine = line(offset);
        const auto [entry, inserted] = locations_.try_emplace(sourceLine, slotMetadata(function_.slots.size()) + locations_.size());
        (void)inserted;
        output_ << ", !dbg !" << entry->second;
    }

    void declare(std::size_t slot, std::size_t offset)
    {
        if (!debug_)
            return;

        const auto* intrinsic = slot < function_.parameterCount && function_.mode == FunctionMode::Construction
            ? "addr" : "declare";
        output_ << "  call void @llvm.dbg." << intrinsic << "(metadata ptr %slot" << slot
                << ", metadata !" << slotMetadata(slot) << ", metadata !DIExpression())";
        location(offset);
        output_ << '\n';
    }

    void emitListScratch(std::size_t elementWords)
    {
        if (elementWords != 0) {
            output_ << "  %ownedList = alloca [2 x i64], align 8\n"
                    << "  %ownedElement = alloca [" << elementWords << " x i64], align 8\n";
        }
    }

    void emitOrderedCollectionScratch(const RuntimeScratch& scratch)
    {
        if (scratch.sequenceValueWords != 0)
            output_ << "  %sequenceView = alloca [3 x i64], align 8\n"
                    << "  %sequenceValue = alloca [" << scratch.sequenceValueWords << " x i64], align 8\n";
        if (scratch.dictionaryCalls)
            output_ << "  %dictionaryView = alloca [2 x i64], align 8\n";
        if (scratch.dictionaryKeyWords != 0)
            output_ << "  %dictionaryKey = alloca [" << scratch.dictionaryKeyWords << " x i64], align 8\n";
        if (scratch.dictionaryValueWords != 0)
            output_ << "  %dictionaryValue = alloca [" << scratch.dictionaryValueWords << " x i64], align 8\n";
    }

    void emitConcurrentScratch()
    {
        if (function_.concurrentDictionaryWords != 0)
            output_ << "  %concurrentDictionaryValue = alloca [" << function_.concurrentDictionaryWords << " x i64], align 8\n";
        if (function_.concurrentSequenceWords != 0)
            output_ << "  %concurrentSequenceValue = alloca [" << function_.concurrentSequenceWords << " x i64], align 8\n";
    }

    void emitParallelScratch()
    {
        if (function_.parallelCaptureWords != 0)
            output_ << "  %parallelCaptures = alloca [" << function_.parallelCaptureWords << " x i64], align 8\n";
        if (function_.parallelCancellationSlots != 0)
            output_ << "  %parallelCancellation = alloca [" << function_.parallelCancellationSlots << " x ptr], align 8\n";
    }

    void emitHostScratch(const RuntimeScratch& scratch)
    {
        if (scratch.hostArguments != 0) {
            output_ << "  %hostArguments = alloca [" << scratch.hostArguments << " x %pipe.host.argument], align 8\n";
            output_ << "  %hostPayload = alloca [" << scratch.hostArgumentWords << " x i64], align 8\n";
        }
        if (scratch.hostResultWords != 0)
            output_ << "  %hostResult = alloca [" << scratch.hostResultWords << " x i64], align 8\n";
    }

    void emitComparisonScratch(std::size_t words)
    {
        if (words == 0)
            return;
        output_ << "  %comparisonLeft = alloca [" << words << " x i64], align 8\n"
                << "  %comparisonRight = alloca [" << words << " x i64], align 8\n";
    }

    void emitTextScratch(const RuntimeScratch& scratch)
    {
        if (scratch.textCalls)
            output_ << "  %textOperand0 = alloca %pipe.text\n"
                       "  %textOperand1 = alloca %pipe.text\n"
                       "  %textOutput = alloca %pipe.text\n";
    }

    void emitByteScratch(const RuntimeScratch& scratch)
    {
        if (scratch.byteCalls)
            output_ << "  %byteOperand0 = alloca %pipe.bytes\n"
                       "  %byteOperand1 = alloca %pipe.bytes\n"
                       "  %byteOutput = alloca %pipe.bytes\n";
    }

    void emitRuntimeEntry()
    {
        output_ << "dispatch:\n  %runtimeInteger = alloca i64\n";
        const auto scratch = runtimeScratch(function_, layouts_);
        emitConcurrentScratch();
        emitParallelScratch();
        emitFixedArrayScratch(scratch.fixedArrayWords);
        emitResultInterfaceScratch(scratch);
        emitArrayScratch(scratch.arrayElementWords);
        emitListScratch(scratch.listElementWords);
        emitComparisonScratch(scratch.comparisonWords);
        if (scratch.capturesError)
            output_ << "  %runtimeError = alloca %pipe.text\n";
        emitHostScratch(scratch);
        emitTextScratch(scratch);
        emitByteScratch(scratch);
        if (scratch.byteFieldWrites)
            output_ << "  %byteField = alloca %pipe.bytes\n";
        if (scratch.recordFieldWords != 0)
            output_ << "  %receiverRecord = alloca [" << scratch.recordFieldWords << " x i64], align 8\n";
        emitOrderedCollectionScratch(scratch);

        if (!function_.continuation()) {
            emitStackSlots();
            if (function_.childResultWords != 0)
                output_ << "  %childResult = alloca [" << function_.childResultWords << " x i64], align 8\n";
            if (hasFaultTargets_)
                output_ << "  %faultTarget = alloca i64\n";
            output_ << "  br label %block0\n";
            return;
        }

        std::size_t word = 0;
        for (std::size_t slot = 0; slot < function_.slots.size(); ++slot) {
            output_ << (debug_ ? "  %savedSlot" : "  %slot") << slot
                    << " = getelementptr i64, ptr %storage, i64 "
                    << word << '\n';
            word += valueWords(function_.slots[slot].type, layouts_);
            if (debug_) {
                // Debug builds mirror durable values in typed stack slots so
                // the debugger can locate them after every dispatch.
                const auto type = storageType(function_.slots[slot].type);
                output_ << "  %slot" << slot << " = alloca " << type << '\n'
                        << "  %restoredSlot" << slot << " = load " << type << ", ptr %savedSlot" << slot << '\n'
                        << "  store " << type << " %restoredSlot" << slot << ", ptr %slot" << slot << '\n';
            }
        }
        for (std::size_t parameter = 0; parameter < function_.parameterCount; ++parameter)
            declare(parameter, function_.offset);

        output_ << "  switch i64 %programCounter, label %runtime_failure [\n    i64 0, label %block0\n";
        std::set<std::size_t> destinations;
        for (const auto& block : function_.blocks) {
            if (const auto* suspension = std::get_if<Suspend>(&block.exit.value()))
                destinations.insert(suspension->resume);
            else if (const auto* call = std::get_if<AwaitCall>(&block.exit.value()))
                destinations.insert(call->resume);
            else if (const auto* host = std::get_if<AwaitHost>(&block.exit.value()))
                destinations.insert(host->resume);
            if (block.faultTarget)
                destinations.insert(block.faultTarget);
        }
        for (const auto destination : destinations)
            output_ << "    i64 " << destination << ", label %block" << destination << '\n';
        output_ << "  ]\n";
    }

    void emitContextualFailure()
    {
        if (hasFaultTargets_) {
            std::set<std::size_t> handlers;
            for (const auto& block : function_.blocks) {
                if (block.faultTarget)
                    handlers.insert(block.faultTarget);
            }
            output_ << "  %activeFault = load i64, ptr %faultTarget\n"
                    << "  switch i64 %activeFault, label %unhandled [\n";
            for (const auto handler : handlers)
                output_ << "    i64 " << handler << ", label %handle" << handler << '\n';
            output_ << "  ]\n";
            for (const auto handler : handlers) {
                output_ << "handle" << handler << ":\n"
                        << "  call void @pipelang_native_v19_route_fault(ptr %runtime)\n"
                        << "  br label %block" << handler << '\n';
            }
            output_ << "unhandled:\n";
        }
        output_ << "  ret i32 1\n";
    }

    void emitStackSlots()
    {
        if (function_.receiverStorageWords != 0)
            output_ << "  %stackReceivers = alloca [" << function_.receiverStorageWords << " x i64], align 8\n";
        for (std::size_t slot = 0; slot < function_.slots.size(); ++slot)
            output_ << "  %slot" << slot << " = alloca " << storageType(function_.slots[slot].type) << '\n';
    }

    void emitResultInterfaceScratch(const RuntimeScratch& scratch)
    {
        if (scratch.resultInterfaceInputWords != 0)
            output_ << "  %resultInterfaceInput = alloca [" << scratch.resultInterfaceInputWords << " x i64], align 8\n";
        if (scratch.resultInterfaceOutputWords != 0)
            output_ << "  %resultInterfaceOutput = alloca [" << scratch.resultInterfaceOutputWords << " x i64], align 8\n";
    }

    void emitBlock(std::size_t index, const Block& block)
    {
        currentFaultTarget_ = block.faultTarget;
        output_ << "block" << index << ":\n";
        if (function_.usesRuntime() && hasFaultTargets_) {
            if (function_.continuation())
                storeStepField("%faultTarget" + std::to_string(index), 6, "i64", std::to_string(block.faultTarget));
            else
                output_ << "  store i64 " << block.faultTarget << ", ptr %faultTarget\n";
        }
        if (index == 0) {
            if (!function_.usesRuntime()) {
                const auto scratch = runtimeScratch(function_, layouts_);
                emitFixedArrayScratch(scratch.fixedArrayWords);
                emitResultInterfaceScratch(scratch);
                emitStackSlots();
                if (function_.construction()) {
                    emitHostScratch(scratch);
                    emitConcurrentScratch();
                    emitParallelScratch();
                    emitArrayScratch(scratch.arrayElementWords);
                    emitListScratch(scratch.listElementWords);
                    emitOrderedCollectionScratch(scratch);
                    emitComparisonScratch(scratch.comparisonWords);
                    emitTextScratch(scratch);
                    emitByteScratch(scratch);
                    if (retainsScopedBytes_ && scratch.recordFieldWords != 0)
                        output_ << "  %receiverRecord = alloca [" << scratch.recordFieldWords << " x i64], align 8\n";
                    output_ << "  %runtimeInteger = alloca i64\n";
                    if (scratch.capturesError)
                        output_ << "  %runtimeError = alloca %pipe.text\n";
                    if (function_.childResultWords != 0)
                        output_ << "  %childResult = alloca [" << function_.childResultWords << " x i64], align 8\n";
                }
            }

            if (debug_ && function_.mode == FunctionMode::Construction) {
                // Describe incoming parameters separately from their later stack slots.
                for (std::size_t parameter = 0; parameter < function_.parameterCount; ++parameter) {
                    const auto type = function_.slots[parameter].type;
                    output_ << "  call void @llvm.dbg.value(metadata " << llvmType(type) << " %argument" << parameter
                            << ", metadata !" << slotMetadata(parameter) << ", metadata !DIExpression())";
                    location(function_.offset);
                    output_ << '\n';
                }
            }

            std::size_t argument = 0;
            for (std::size_t parameter = 0; parameter < function_.parameterCount; ++parameter) {
                const auto type = function_.slots[parameter].type;
                if (function_.continuation() && type == ScalarType::Receiver) {
                    output_ << "  store ptr %receiver, ptr %slot" << parameter;
                } else if (function_.continuation()) {
                    const auto address = "%incoming" + std::to_string(argument);
                    const auto indirect = indirectArgument(type);
                    const auto member = type == ScalarType::CancellationToken || indirect ? 1 : 0;
                    const auto layout = storageType(type);
                    output_ << "  " << address << " = getelementptr %pipe.argument, ptr %arguments, i64 "
                            << argument << ", i32 " << member << '\n';
                    if (indirect)
                        output_ << "  " << address << ".payload = load ptr, ptr " << address << '\n';
                    if (parameter == 0 && takesProcessArguments(function_)) {
                        const auto sequence = runtimeCalls_++;
                        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_import_process_arguments"
                                << "(ptr %runtime, ptr " << ownedShapeName(type) << ", ptr " << address
                                << ".payload, ptr %slot" << parameter << ')';
                        checkRuntimeStatus(sequence, function_.offset);
                    } else {
                        output_ << "  " << address << ".value = load " << layout << ", ptr " << address
                                << (indirect ? ".payload" : "") << '\n'
                                << "  store " << layout << ' ' << address << ".value, ptr %slot" << parameter;
                    }
                    ++argument;
                } else {
                    const auto incoming = "%argument" + std::to_string(parameter);
                    auto stored = incoming;
                    if (type == ScalarType::Boolean) {
                        stored += ".byte";
                        output_ << "  " << stored << " = zext i1 " << incoming << " to i8\n";
                    }
                    output_ << "  store " << storageType(type) << ' ' << stored << ", ptr %slot" << parameter;
                }
                output_ << '\n';
                if (!function_.continuation())
                    declare(parameter, function_.offset);
            }
            if (debug_ && function_.parameterCount != 0) {
                // Give declaration breakpoints an instruction after the
                // argument stores, even when the first body row starts earlier.
                output_ << "  call void asm sideeffect \"nop\", \"~{memory}\"()";
                location(function_.offset);
                output_ << '\n';
            }
        }

        for (const auto& instruction : block.instructions) {
            std::visit([&](const auto& operation) { emitInstruction(operation, instruction.offset); }, instruction.operation);
        }

        std::visit([&](const auto& exit) { emitExit(exit, block.exitOffset); }, block.exit.value());
    }

    template<class Operation>
    void emitInstruction(const Operation& operation, std::size_t offset)
    {
        emitOperation(operation, offset);
        location(offset);
        output_ << '\n';
    }

    std::string integerArgument(const std::optional<Operand>& argument, std::size_t offset)
    {
        if (!argument)
            return "0";
        if (argument->type != ScalarType::Boolean)
            return operandName(*argument);

        const auto name = "%runtimeBoolean" + std::to_string(runtimeBooleans_++);
        output_ << "  " << name << " = zext i1 " << operandName(*argument) << " to i64";
        location(offset);
        output_ << '\n';
        return name;
    }

    void emitInstruction(const ListAppend& append, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto listType = llvmType(append.list.type);
        const auto elementType = layouts_[append.list.type.layout].fields.front().type;
        const auto* operation = function_.usesRuntime() ? "list_append(ptr %runtime" :
            "hosted_list_append(ptr %arena";
        const auto element = integerField(append.element, "%append" + std::to_string(sequence), offset);
        output_ << "  store " << listType << ' ' << operandName(append.list) << ", ptr %ownedList\n"
                << "  store " << fieldStorageType(append.element.type) << ' ' << element << ", ptr %ownedElement\n"
                << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", ptr %ownedList, ptr "
                << ownedShapeName(elementType) << ", ptr %ownedElement, ptr %ownedList)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(append.result) << " = load " << listType << ", ptr %ownedList";
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const SetCall& call, std::size_t offset)
    {
        using RuntimeOperation = native_app_v19::SetOperation;
        auto operation = RuntimeOperation::Contains;
        switch (call.operation) {
        case SetOperation::Contains: break;
        case SetOperation::Add: operation = RuntimeOperation::Add; break;
        case SetOperation::Remove: operation = RuntimeOperation::Remove; break;
        case SetOperation::ToList: operation = RuntimeOperation::ToList; break;
        }
        const auto sequence = runtimeCalls_++;
        const auto* nativeCall = function_.usesRuntime() ? "set_operation(ptr %runtime" :
            "hosted_set_operation(ptr %arena";
        const auto setType = llvmType(call.set.type);
        const auto keyType = layouts_[call.set.type.layout].fields.front().type;
        if (call.key) {
            const auto key = integerField(*call.key, "%setKey" + std::to_string(sequence), offset);
            output_ << "  store " << fieldStorageType(keyType) << ' ' << key << ", ptr %ownedElement\n";
        }
        output_ << "  store " << setType << ' ' << operandName(call.set) << ", ptr %ownedList\n"
                << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << nativeCall << ", i32 "
                << static_cast<std::int32_t>(operation) << ", ptr %ownedList, ptr " << ownedShapeName(keyType)
                << ", ptr " << (call.key ? "%ownedElement" : "null") << ", ptr %ownedList, ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        const auto result = registerName(call.result);
        if (call.updated)
            output_ << "  " << registerName(*call.updated) << " = load " << setType << ", ptr %ownedList\n";
        if (call.operation == SetOperation::ToList) {
            output_ << "  " << result << " = load " << llvmType(call.resultType) << ", ptr %ownedList";
        } else {
            output_ << "  " << result << ".word = load i64, ptr %runtimeInteger\n"
                    << "  " << result << " = trunc i64 " << result << ".word to i1";
        }
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const CollectionCall& call, std::size_t offset)
    {
        const auto name = registerName(call.result);
        if (call.operation == CollectionOperation::Count) {
            output_ << "  " << name << " = extractvalue " << llvmType(call.receiver.type) << ' '
                    << operandName(call.receiver) << ", 1";
            location(offset);
            output_ << '\n';
            return;
        }
        const auto sequence = runtimeCalls_++;
        const auto type = layouts_[call.receiver.type.layout].fields.front().type;
        const auto* operation = function_.usesRuntime() ? "list_read(ptr %runtime" :
            "hosted_list_read(ptr %arena";
        output_ << "  store " << llvmType(call.receiver.type) << ' ' << operandName(call.receiver) << ", ptr %ownedList\n"
                << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", ptr %ownedList, ptr "
                << ownedShapeName(type) << ", i64 " << operandName(*call.index) << ", ptr %ownedElement)";
        checkConstructionOperation(sequence, offset);
        if (type == ScalarType::Boolean) {
            output_ << "  " << name << ".word = load i64, ptr %ownedElement\n"
                    << "  " << name << " = trunc i64 " << name << ".word to i1";
        } else {
            output_ << "  " << name << " = load " << llvmType(type) << ", ptr %ownedElement";
        }
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const SequenceCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* nativeCall = function_.usesRuntime() ? "sequence(ptr %runtime" :
            "hosted_sequence(ptr %arena";
        const auto view = llvmType(call.sequence.type);
        if (call.argument) {
            const auto value = integerField(*call.argument, "%sequenceItem" + std::to_string(sequence), offset);
            output_ << "  store " << fieldStorageType(call.argument->type) << ' ' << value << ", ptr %sequenceValue\n";
        }
        output_ << "  store " << view << ' ' << operandName(call.sequence) << ", ptr %sequenceView\n"
                << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << nativeCall << ", i32 "
                << static_cast<std::int32_t>(call.operation) << ", ptr " << ownedShapeName(call.sequence.type)
                << ", ptr %sequenceView, ptr " << (call.argument ? "%sequenceValue" : "null")
                << ", i64 " << (call.index ? operandName(*call.index) : "0")
                << ", ptr " << (call.result ? "%sequenceValue" : "null") << ')';
        checkConstructionOperation(sequence, offset);
        if (call.updated) {
            output_ << "  " << registerName(*call.updated) << " = load " << view << ", ptr %sequenceView";
            location(offset);
            output_ << '\n';
        }
        if (call.result) {
            const auto name = operandName(*call.result);
            const auto type = call.result->type;
            if (type == ScalarType::Boolean) {
                output_ << "  " << name << ".word = load i64, ptr %sequenceValue\n"
                        << "  " << name << " = trunc i64 " << name << ".word to i1";
            } else {
                output_ << "  " << name << " = load " << llvmType(type) << ", ptr %sequenceValue";
            }
            location(offset);
            output_ << '\n';
        }
    }

    void emitInstruction(const DictionaryCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* nativeCall = function_.usesRuntime() ? "dictionary(ptr %runtime" :
            "hosted_dictionary(ptr %arena";
        const auto view = llvmType(call.dictionary.type);
        const auto stage = [&](const std::optional<Operand>& operand, const std::string& storage) {
            if (!operand)
                return;
            const auto argument = integerField(*operand, storage + std::to_string(sequence), offset);
            output_ << "  store " << fieldStorageType(operand->type) << ' ' << argument << ", ptr " << storage << '\n';
        };
        stage(call.key, "%dictionaryKey");
        stage(call.value, "%dictionaryValue");

        output_ << "  store " << view << ' ' << operandName(call.dictionary) << ", ptr %dictionaryView\n"
                << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << nativeCall << ", i32 "
                << static_cast<std::int32_t>(call.operation) << ", ptr " << ownedShapeName(call.dictionary.type)
                << ", ptr %dictionaryView, ptr " << (call.key ? "%dictionaryKey" : "null")
                << ", ptr " << (call.value ? "%dictionaryValue" : "null")
                << ", ptr " << (call.projection ? ownedShapeName(*call.projection) : "null")
                << ", i64 " << (call.index ? operandName(*call.index) : "0")
                << ", ptr " << (call.result ? "%dictionaryValue" : "null") << ')';
        checkConstructionOperation(sequence, offset);
        if (call.updated) {
            output_ << "  " << registerName(*call.updated) << " = load " << view << ", ptr %dictionaryView";
            location(offset);
            output_ << '\n';
        }
        if (call.result) {
            const auto name = operandName(*call.result);
            const auto type = call.result->type;
            if (type == ScalarType::Boolean) {
                output_ << "  " << name << ".word = load i64, ptr %dictionaryValue\n"
                        << "  " << name << " = trunc i64 " << name << ".word to i1";
            } else {
                output_ << "  " << name << " = load " << llvmType(type) << ", ptr %dictionaryValue";
            }
            location(offset);
            output_ << '\n';
        }
    }

    void emitInstruction(const ArrayCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto name = registerName(call.result);
        const auto view = llvmType(call.array);
        if (call.receiver)
            output_ << "  store " << view << ' ' << operandName(*call.receiver) << ", ptr %arrayView\n";
        if (call.value) {
            const auto value = integerField(*call.value, name + ".item", offset);
            output_ << "  store " << fieldStorageType(call.value->type) << ' ' << value << ", ptr %arrayElement\n";
        }
        const bool reads = call.operation == native_app_v19::ArrayOperation::At;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_"
                << (function_.usesRuntime() ? "array(ptr %runtime, i32 " : "hosted_initialization_array(ptr %arena, i32 ")
                << static_cast<std::int32_t>(call.operation) << ", ptr " << ownedShapeName(call.array)
                << ", ptr %arrayView, i64 " << operandName(call.index)
                << ", ptr " << (call.value ? "%arrayElement" : "null")
                << ", ptr " << (reads ? "%arrayElement" : "null") << ')';
        checkConstructionOperation(sequence, offset);
        const auto type = reads ? layouts_[call.array.layout].fields.front().type : call.array;
        if (type == ScalarType::Boolean) {
            output_ << "  " << name << ".word = load i64, ptr %arrayElement\n"
                    << "  " << name << " = trunc i64 " << name << ".word to i1";
        } else {
            output_ << "  " << name << " = load " << llvmType(type)
                    << ", ptr " << (reads ? "%arrayElement" : "%arrayView");
        }
        location(offset);
        output_ << '\n';
    }

    void emitArrayScratch(std::size_t elementWords)
    {
        if (elementWords != 0)
            output_ << "  %arrayView = alloca [2 x i64], align 8\n"
                    << "  %arrayElement = alloca [" << elementWords << " x i64], align 8\n";
    }

    void emitFixedArrayScratch(std::size_t words)
    {
        if (words != 0)
            output_ << "  %fixedArrayScratch = alloca [" << words << " x i64], align 8\n";
    }

    void validateFixedArray(NativeType type, std::size_t offset)
    {
        const auto status = runtimeCalls_++;
        const auto* operation = function_.usesRuntime()
            ? "validate_fixed_array(ptr %runtime"
            : "hosted_validate_fixed_array(ptr %arena";
        output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                << ownedShapeName(type) << ", ptr %fixedArrayScratch)";
        checkConstructionOperation(status, offset);
    }

    void emitInstruction(const FillFixedArray& fill, std::size_t offset)
    {
        const auto name = registerName(fill.result);
        const auto label = std::to_string(fill.result.index);
        const auto start = "fixed.fill.start" + label;
        const auto loop = "fixed.fill.loop" + label;
        const auto done = "fixed.fill.done" + label;
        const auto element = fieldStorageType(fill.initial.type);
        const auto initial = integerField(fill.initial, name + ".word", offset);
        const auto extent = layouts_[fill.array.layout].fixedExtent;
        output_ << "  br label %" << start;
        location(offset);
        output_ << '\n' << start << ":\n"
                << "  br label %" << loop << '\n' << loop << ":\n"
                << "  " << name << ".index = phi i64 [0, %" << start << "], [" << name << ".next, %" << loop << "]\n"
                << "  " << name << ".element = getelementptr " << element << ", ptr %fixedArrayScratch, i64 "
                << name << ".index\n"
                << "  store " << element << ' ' << initial << ", ptr " << name << ".element\n"
                << "  " << name << ".next = add i64 " << name << ".index, 1\n"
                << "  " << name << ".complete = icmp eq i64 " << name << ".next, " << extent << '\n'
                << "  br i1 " << name << ".complete, label %" << done << ", label %" << loop << '\n'
                << done << ":\n";
        if (fill.validateOwnership)
            validateFixedArray(fill.array, offset);
        output_ << "  " << name << " = load " << llvmType(fill.array) << ", ptr %fixedArrayScratch";
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const FixedArrayIndex& access, std::size_t offset)
    {
        const auto name = registerName(access.result);
        const auto next = "fixed.index.valid" + std::to_string(access.result.index);
        const auto array = llvmType(access.array.type);
        const auto element = fieldStorageType(access.element);
        const auto index = operandName(access.index);
        const auto extent = layouts_[access.array.type.layout].fixedExtent;
        output_ << "  " << name << ".valid = icmp ult i64 " << index << ", " << extent << '\n'
                << "  br i1 " << name << ".valid, label %" << next << ", label %"
                << sourceGuardLabel("invalid_fixed_array_index", offset);
        location(offset);
        output_ << '\n' << next << ":\n"
                << "  store " << array << ' ' << operandName(access.array) << ", ptr %fixedArrayScratch\n"
                << "  " << name << ".element = getelementptr " << element << ", ptr %fixedArrayScratch, i64 "
                << index << '\n';
        if (access.replacement) {
            const auto item = integerField(*access.replacement, name + ".word", offset);
            output_ << "  store " << element << ' ' << item << ", ptr " << name << ".element\n";
            if (access.validateOwnership)
                validateFixedArray(access.array.type, offset);
            output_ << "  " << name << " = load " << array << ", ptr %fixedArrayScratch";
        } else if (access.element == ScalarType::Boolean) {
            output_ << "  " << name << ".word = load i64, ptr " << name << ".element\n"
                    << "  " << name << " = trunc i64 " << name << ".word to i1";
        } else {
            output_ << "  " << name << " = load " << element << ", ptr " << name << ".element";
        }
        location(offset);
        output_ << '\n';
    }

    void checkRuntimeStatus(std::size_t sequence, std::size_t offset)
    {
        location(offset);
        output_ << "\n  %succeeded" << sequence << " = icmp eq i32 %status" << sequence << ", 0\n"
                << "  br i1 %succeeded" << sequence << ", label %runtime_continue" << sequence;
        if (traceFunction_.empty()) {
            output_ << ", label %runtime_failure\n";
        } else {
            output_ << ", label %trace_failure" << sequence << "\ntrace_failure" << sequence << ":\n";
            emitSourceNote("failure", offset);
            output_ << "  br label %runtime_failure\n";
        }
        output_ << "runtime_continue" << sequence << ":\n";
    }

    void emitSourceNote(std::string_view event, std::size_t offset)
    {
        if (traceFunction_.empty())
            return;
        output_ << "  call void @pipelang_native_v19_note_" << event << "(ptr %runtime, ptr " << traceFunction_
                << ", ptr " << traceFile_ << ", i64 " << line(offset) << ')';
        location(offset);
        output_ << '\n';
    }

    std::string sourceGuardLabel(std::string_view diagnostic, std::size_t offset)
    {
        if (traceFunction_.empty() &&
            (function_.usesRuntime() || (currentFaultTarget_ == 0 &&
             diagnostic != "invalid_optional_value" && diagnostic != "invalid_fixed_array_index")))
            return std::string(diagnostic);
        const auto index = sourceGuards_.size();
        sourceGuards_.push_back({diagnostic, offset, currentFaultTarget_});
        return "source_guard_" + std::to_string(index);
    }

    void emitHostedFailure(std::string_view status, std::size_t handler)
    {
        if (handler == 0) {
            output_ << "  ret i32 " << status << '\n';
        } else {
            output_ << "  call void @pipelang_native_v19_hosted_route_fault(ptr %arena, i32 " << status << ")\n"
                    << "  br label %block" << handler << '\n';
        }
    }

    void emitHostedGuards()
    {
        for (std::size_t index = 0; index < sourceGuards_.size(); ++index) {
            const auto& guard = sourceGuards_[index];
            output_ << "source_guard_" << index << ":\n";
            if (guard.diagnostic == "invalid_result_value" || guard.diagnostic == "invalid_optional_value" ||
                guard.diagnostic == "invalid_fixed_array_index") {
                const auto status = "%guardStatus" + std::to_string(index);
                output_ << "  " << status << " = call i32 @pipelang_native_v19_hosted_"
                        << guard.diagnostic << "(ptr %arena)\n";
                emitHostedFailure(status, guard.handler);
            } else if (guard.diagnostic == "receiver_allocation_failed") {
                emitHostedFailure("1", guard.handler);
            } else if (guard.diagnostic == "uninitialized_receiver") {
                emitHostedFailure("2", guard.handler);
            } else if (guard.diagnostic == "uninitialized_context_owner") {
                emitHostedFailure("4", guard.handler);
            } else {
                throw std::logic_error("unknown native hosted construction guard");
            }
        }
    }

    void checkConstructionOperation(std::size_t sequence, std::size_t offset)
    {
        if (function_.usesRuntime()) {
            checkRuntimeStatus(sequence, offset);
            return;
        }
        location(offset);
        output_ << "\n  %constructedValue" << sequence << " = icmp eq i32 %status" << sequence << ", 0\n"
                << "  br i1 %constructedValue" << sequence << ", label %construction.next" << sequence;
        if (currentFaultTarget_ == 0) {
            output_ << ", label %hosted_construction_failed\n";
        } else {
            output_ << ", label %construction.fault" << sequence << "\nconstruction.fault" << sequence << ":\n";
            emitHostedFailure("%status" + std::to_string(sequence), currentFaultTarget_);
        }
        output_ << "construction.next" << sequence << ":\n";
    }

    void emitInstruction(const BudgetStep& step, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_";
        if (!function_.usesRuntime())
            output_ << "charge_hosted_initialization(ptr %arena, i64 " << step.count << ')';
        else if (step.count == 1)
            output_ << "step(ptr %runtime)";
        else
            output_ << "charge(ptr %runtime, i64 " << step.count << ')';
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const BeginCleanup&, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_"
                << (function_.usesRuntime() ? "begin_cleanup(ptr %runtime)" : "hosted_begin_cleanup(ptr %arena)");
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const EndCleanup&, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_"
                << (function_.usesRuntime() ? "end_cleanup(ptr %runtime)" : "hosted_end_cleanup(ptr %arena)");
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const CaptureError& capture, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence
                << " = call i32 @pipelang_native_v19_"
                << (function_.usesRuntime() ? "capture_error(ptr %runtime" : "hosted_capture_error(ptr %arena")
                << ", ptr %runtimeError)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(capture.result) << " = load %pipe.text, ptr %runtimeError";
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const RaiseError& raise, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto name = "%raised" + std::to_string(sequence);
        const auto error = operandName(raise.error);
        const auto* operation = function_.usesRuntime()
            ? "pipelang_native_v19_raise_error(ptr %runtime" : "pipelang_native_v19_hosted_raise_error(ptr %arena";
        output_ << "  " << name << ".bytes = extractvalue %pipe.text " << error << ", 0\n"
                << "  " << name << ".size = extractvalue %pipe.text " << error << ", 1\n"
                << "  %status" << sequence << " = call i32 @" << operation << ", ptr "
                << name << ".bytes, i64 " << name << ".size)";
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const CancellationRequested& query, std::size_t offset)
    {
        const auto name = registerName(query.result);
        output_ << "  " << name << ".status = call i32 @pipelang_native_v19_cancellation_requested(ptr "
                << operandName(query.token) << ")\n"
                << "  " << name << " = icmp ne i32 " << name << ".status, 0";
        location(offset);
        output_ << '\n';
    }

    static native_app_v19::IntegerOperation integerOperation(IntegerOperation operation)
    {
        using Abi = native_app_v19::IntegerOperation;
        switch (operation) {
        case IntegerOperation::Add: return Abi::Add;
        case IntegerOperation::Subtract: return Abi::Subtract;
        case IntegerOperation::Multiply: return Abi::Multiply;
        case IntegerOperation::Divide: return Abi::Divide;
        case IntegerOperation::Remainder: return Abi::Remainder;
        }
        throw std::logic_error("unrecognized native integer operation");
    }

    void emitOperation(const CheckedInteger& operation, std::size_t offset)
    {
        if (operation.knownValue) {
            output_ << "  " << registerName(operation.result) << " = add i64 0, " << *operation.knownValue;
            return;
        }
        const auto sequence = runtimeCalls_++;
        const auto* function = function_.usesRuntime() ? "integer(ptr %runtime" : "hosted_integer(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << function << ", i32 "
                << static_cast<int>(integerOperation(operation.operation)) << ", i64 " << operandName(operation.left)
                << ", i64 " << operandName(operation.right) << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(operation.result) << " = load i64, ptr %runtimeInteger";
    }

    void emitOperation(const UInt32Compute& compute, std::size_t offset)
    {
        using Operation = UInt32Operation;
        using Abi = native_app_v19::UInt32Operation;
        static constexpr std::array checked{
            Abi::Add, Abi::Subtract, Abi::Multiply, Abi::Divide, Abi::Remainder,
            Abi::ShiftLeft, Abi::ShiftRight, Abi::FromInt
        };
        const auto index = static_cast<std::size_t>(compute.operation);
        const auto left = operandName(compute.left);
        const auto right = operandName(compute.right);
        const auto result = registerName(compute.result);
        if (index < checked.size()) {
            const auto sequence = runtimeCalls_++;
            const auto* operation = function_.usesRuntime()
                ? "uint32(ptr %runtime"
                : "hosted_uint32(ptr %arena";
            output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i32 "
                    << static_cast<int>(checked[index]) << ", i64 " << left << ", i64 " << right << ", ptr %runtimeInteger)";
            checkConstructionOperation(sequence, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }

        const char* opcode;
        switch (compute.operation) {
        case Operation::And: opcode = "and"; break;
        case Operation::Or: opcode = "or"; break;
        case Operation::Xor: opcode = "xor"; break;
        case Operation::WrappingAdd: opcode = "add"; break;
        case Operation::WrappingSubtract: opcode = "sub"; break;
        case Operation::WrappingMultiply: opcode = "mul"; break;
        default: throw std::logic_error("unknown native uint32 operation");
        }
        // Frame and owned storage keep unsigned values zero-extended to one word.
        if (compute.operation >= Operation::WrappingAdd) {
            output_ << "  " << result << ".word = " << opcode << " i64 " << left << ", " << right << '\n'
                    << "  " << result << " = and i64 " << result << ".word, 4294967295";
        } else {
            output_ << "  " << result << " = " << opcode << " i64 " << left << ", " << right;
        }
    }

    void decodeFloating(const Operand& operand, const std::string& name)
    {
        auto bits = operandName(operand);
        if (operand.type == ScalarType::Float32) {
            output_ << "  " << name << ".bits = trunc i64 " << bits << " to i32\n";
            bits = name + ".bits";
        }
        output_ << "  " << name << " = bitcast "
                << (operand.type == ScalarType::Float32 ? "i32" : "i64") << ' ' << bits
                << " to " << llvmFloatingType(operand.type) << '\n';
    }

    void emitOperation(const FloatingCompute& compute, std::size_t)
    {
        const auto result = registerName(compute.result);
        const auto type = compute.left.type;
        if (compute.operation == FloatingArithmetic::Negate) {
            output_ << "  " << result << " = xor i64 " << operandName(compute.left) << ", "
                    << (type == ScalarType::Float32 ? "2147483648" : "-9223372036854775808");
            return;
        }
        const char* opcode;
        switch (compute.operation) {
        case FloatingArithmetic::Add: opcode = "fadd"; break;
        case FloatingArithmetic::Subtract: opcode = "fsub"; break;
        case FloatingArithmetic::Multiply: opcode = "fmul"; break;
        case FloatingArithmetic::Divide: opcode = "fdiv"; break;
        default: throw std::logic_error("unknown native floating arithmetic operation");
        }
        decodeFloating(compute.left, result + ".left");
        decodeFloating(compute.right.value(), result + ".right");
        const auto* floating = llvmFloatingType(type);
        output_ << "  " << result << ".fp = call " << floating << " @llvm.experimental.constrained." << opcode
                << '.' << floatingSuffix(type) << '(' << floating << ' ' << result << ".left, "
                << floating << ' ' << result << ".right, metadata !\"round.dynamic\", metadata !\"fpexcept.strict\")";
        if (type == ScalarType::Float32) {
            output_ << "\n  " << result << ".bits = bitcast float " << result << ".fp to i32"
                    << "\n  " << result << " = zext i32 " << result << ".bits to i64";
        } else {
            output_ << "\n  " << result << " = bitcast double " << result << ".fp to i64";
        }
    }

    void emitActiveNarrow(const FloatingCall& call, std::size_t offset)
    {
        constexpr std::uint32_t signBit = 0x80000000U;
        constexpr std::uint32_t infinity = 0x7f800000U;
        constexpr std::uint32_t quietNaN = 0x7fc00000U;
        const auto value = registerName(call.result);
        decodeFloating(call.arguments.at(0), value + ".input");
        output_ << "  " << value << ".narrowed = call float @llvm.experimental.constrained.fptrunc.f32.f64(double "
                << value << ".input, metadata !\"round.dynamic\", metadata !\"fpexcept.strict\")";
        location(offset);
        // Keep the hardware exception state and canonicalize NaNs after the conversion.
        output_ << '\n'
                << "  " << value << ".bits = bitcast float " << value << ".narrowed to i32\n"
                << "  " << value << ".absolute = and i32 " << value << ".bits, " << (signBit - 1) << '\n'
                << "  " << value << ".isNaN = icmp ugt i32 " << value << ".absolute, " << infinity << '\n'
                << "  " << value << ".sign = and i32 " << value << ".bits, " << signBit << '\n'
                << "  " << value << ".nan = or i32 " << value << ".sign, " << quietNaN << '\n'
                << "  " << value << ".canonical = select i1 " << value << ".isNaN, i32 " << value
                << ".nan, i32 " << value << ".bits\n"
                << "  " << value << " = zext i32 " << value << ".canonical to i64";
    }

    void emitActiveWiden(const FloatingCall& call, std::size_t offset)
    {
        constexpr std::int64_t signBit = std::numeric_limits<std::int64_t>::min();
        constexpr std::int64_t absoluteMask = std::numeric_limits<std::int64_t>::max();
        constexpr std::int64_t infinity = 0x7ff0000000000000;
        constexpr std::int64_t quietNaN = 0x7ff8000000000000;
        const auto value = registerName(call.result);
        decodeFloating(call.arguments.at(0), value + ".input");
        output_ << "  " << value << ".widened = call double @llvm.experimental.constrained.fpext.f64.f32(float "
                << value << ".input, metadata !\"fpexcept.strict\")";
        location(offset);
        output_ << '\n'
                << "  " << value << ".bits = bitcast double " << value << ".widened to i64\n"
                << "  " << value << ".absolute = and i64 " << value << ".bits, " << absoluteMask << '\n'
                << "  " << value << ".isNaN = icmp ugt i64 " << value << ".absolute, " << infinity << '\n'
                << "  " << value << ".sign = and i64 " << value << ".bits, " << signBit << '\n'
                << "  " << value << ".nan = or i64 " << value << ".sign, " << quietNaN << '\n'
                << "  " << value << " = select i1 " << value << ".isNaN, i64 " << value
                << ".nan, i64 " << value << ".bits";
    }

    void emitActiveFromInteger(const FloatingCall& call, std::size_t offset)
    {
        const auto value = registerName(call.result);
        const auto type = call.resultType;
        output_ << "  " << value << ".converted = call " << llvmFloatingType(type)
                << " @llvm.experimental.constrained.sitofp." << floatingSuffix(type) << ".i64(i64 "
                << operandName(call.arguments.at(0))
                << ", metadata !\"round.dynamic\", metadata !\"fpexcept.strict\")";
        location(offset);
        if (type == ScalarType::Float32) {
            output_ << "\n  " << value << ".bits = bitcast float " << value << ".converted to i32"
                    << "\n  " << value << " = zext i32 " << value << ".bits to i64";
        } else {
            output_ << "\n  " << value << " = bitcast double " << value << ".converted to i64";
        }
    }

    void emitOperation(const FloatingCall& call, std::size_t offset)
    {
        if (activeHostFloating_ && call.operation == native_app_v19::FloatingBuiltin::Narrow) {
            emitActiveNarrow(call, offset);
            return;
        }
        if (function_.mode == FunctionMode::Scalar) {
            using Builtin = native_app_v19::FloatingBuiltin;
            if (activeHostFloating_ && call.operation == Builtin::Widen) {
                emitActiveWiden(call, offset);
                return;
            }
            if (activeHostFloating_ && call.operation == Builtin::FromInt) {
                emitActiveFromInteger(call, offset);
                return;
            }
            throw std::logic_error("floating operation has no scalar runtime");
        }

        const auto sequence = runtimeCalls_++;
        const auto output = registerName(call.result);
        const auto* operation = function_.usesRuntime()
            ? "floating(ptr %runtime"
            : "hosted_floating(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i32 "
                << static_cast<int>(call.operation) << ", i32 " << (call.floatingType == ScalarType::Float32);
        for (std::size_t index = 0; index < 3; ++index)
            output_ << ", i64 " << (index < call.arguments.size() ? operandName(call.arguments[index]) : "0");
        output_ << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        if (call.resultType == ScalarType::Boolean) {
            output_ << "  " << output << ".word = load i64, ptr %runtimeInteger\n"
                    << "  " << output << " = trunc i64 " << output << ".word to i1";
        } else {
            output_ << "  " << output << " = load i64, ptr %runtimeInteger";
        }
    }

    void emitInstruction(const SynchronizationOperation& operation, std::size_t offset)
    {
        std::visit([&](const auto& sync) { emitInstruction(sync, offset); }, operation);
    }

    void emitInstruction(const CreateContextOwner& context, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* destination = function_.usesRuntime()
            ? "pipelang_native_v19_create_context_owner(ptr %runtime"
            : "pipelang_native_v19_create_hosted_context_owner(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @" << destination << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(context.result) << " = load i64, ptr %runtimeInteger";
    }

    void emitInstruction(const ContextOwnerCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence
                << " = call i32 @" << (function_.usesRuntime()
                    ? "pipelang_native_v19_context_owner_call(ptr %runtime, i64 "
                    : "pipelang_native_v19_hosted_context_owner_call(ptr %arena, i64 ")
                << operandName(call.receiver) << ", i32 " << static_cast<std::int32_t>(call.operation) << ')';
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const CreateSemaphore& semaphore, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_semaphore(ptr %runtime, i64 "
                << semaphore.site << ", i64 " << operandName(semaphore.initial) << ", i64 "
                << (semaphore.maximum ? operandName(*semaphore.maximum) : "0") << ", i32 "
                << (semaphore.maximum ? 1 : 0) << ')';
        checkRuntimeStatus(sequence, offset);
    }

    void emitInstruction(const CreateSharedSemaphore& semaphore, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* destination = function_.usesRuntime()
            ? "pipelang_native_v19_create_semaphore(ptr %runtime, i64 "
            : "pipelang_native_v19_create_hosted_semaphore(ptr %arena, i64 ";
        output_ << "  %status" << sequence << " = call i32 @" << destination
                << operandName(semaphore.initial) << ", i64 "
                << (semaphore.maximum ? operandName(*semaphore.maximum) : "0") << ", i32 "
                << (semaphore.maximum ? 1 : 0) << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(semaphore.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const ShareSemaphore& share, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_share_semaphore(ptr %runtime, i64 "
                << operandName(share.semaphore) << ", ptr %runtimeInteger)";
        checkRuntimeStatus(sequence, offset);
        output_ << "  " << registerName(share.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const ReadSemaphoreCount& read, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "semaphore_count(ptr %runtime" :
            "hosted_semaphore_count(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(read.semaphore) << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(read.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const ReleaseSemaphore& release, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "release_semaphore(ptr %runtime" :
            "hosted_release_semaphore(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(release.semaphore) << ", i64 " << operandName(release.count) << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(release.result) << " = load i64, ptr %runtimeInteger\n";
    }

    native_app_v19::ConcurrentSequenceKind concurrentSequenceKind(NativeType type) const
    {
        using Kind = native_app_v19::ConcurrentSequenceKind;
        return layouts_[type.layout].name.starts_with("ConcurrentQueue<") ? Kind::Queue : Kind::Stack;
    }

    void emitInstruction(const CreateConcurrentDictionary& create, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto& parts = layouts_[create.type.layout].fields;
        const auto* destination = function_.usesRuntime()
            ? "pipelang_native_v19_create_concurrent_dictionary(ptr %runtime, ptr "
            : "pipelang_native_v19_create_hosted_concurrent_dictionary(ptr %arena, ptr ";
        output_ << "  %status" << sequence << " = call i32 @" << destination
                << ownedShapeName(parts[0].type) << ", ptr " << ownedShapeName(parts[1].type) << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(create.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const ConcurrentDictionaryCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "call_concurrent_dictionary(ptr %runtime" :
            "hosted_call_concurrent_dictionary(ptr %arena";
        const auto base = "%concurrentDictionary" + std::to_string(sequence);
        const auto& parts = layouts_[call.receiver.type.layout].fields;
        std::vector<std::string> addresses;
        std::size_t cursor = 0;
        for (const auto& input : call.inputs) {
            const auto address = base + ".input" + std::to_string(addresses.size());
            const auto value = integerField(input, address + ".word", offset);
            const auto type = input.type == ScalarType::Boolean ? "i64" : llvmType(input.type);
            output_ << "  " << address << " = getelementptr i64, ptr %concurrentDictionaryValue, i64 " << cursor
                    << "\n  store " << type << ' ' << value << ", ptr " << address << '\n';
            addresses.push_back(address);
            cursor += valueWords(input.type, layouts_);
        }
        addresses.resize(3, "null");

        auto result = std::string("null");
        auto shape = std::string("null");
        if (call.output) {
            result = base + ".result";
            shape = ownedShapeName(call.output->type);
            output_ << "  " << result << " = getelementptr i64, ptr %concurrentDictionaryValue, i64 " << cursor << '\n';
        }
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(call.receiver) << ", i32 " << static_cast<std::int32_t>(call.operation)
                << ", ptr " << ownedShapeName(parts[0].type) << ", ptr " << ownedShapeName(parts[1].type)
                << ", ptr " << addresses[0] << ", ptr " << addresses[1] << ", ptr " << addresses[2]
                << ", ptr " << shape << ", ptr " << result << ')';
        checkConstructionOperation(sequence, offset);
        if (!call.output)
            return;

        const auto name = operandName(*call.output);
        if (call.output->type == ScalarType::Boolean) {
            output_ << "  " << name << ".word = load i64, ptr " << result << '\n'
                    << "  " << name << " = trunc i64 " << name << ".word to i1\n";
        } else {
            output_ << "  " << name << " = load " << llvmType(call.output->type) << ", ptr " << result << '\n';
        }
    }

    void emitInstruction(const CreateConcurrentSequence& create, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto element = layouts_[create.type.layout].fields.front().type;
        const auto* destination = function_.usesRuntime()
            ? "pipelang_native_v19_create_concurrent_sequence(ptr %runtime, i32 "
            : "pipelang_native_v19_create_hosted_concurrent_sequence(ptr %arena, i32 ";
        output_ << "  %status" << sequence << " = call i32 @" << destination
                << static_cast<std::int32_t>(concurrentSequenceKind(create.type)) << ", ptr " << ownedShapeName(element)
                << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(create.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const ConcurrentSequenceCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "call_concurrent_sequence(ptr %runtime" :
            "hosted_call_concurrent_sequence(ptr %arena";
        const auto element = layouts_[call.receiver.type.layout].fields.front().type;
        if (call.input) {
            const auto input = integerField(*call.input, "%concurrentInput" + std::to_string(sequence), offset);
            const auto type = call.input->type == ScalarType::Boolean ? "i64" : llvmType(call.input->type);
            output_ << "  store " << type << ' ' << input << ", ptr %concurrentSequenceValue\n";
        }
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(call.receiver) << ", i32 " << static_cast<std::int32_t>(call.operation)
                << ", ptr " << ownedShapeName(element) << ", ptr " << (call.input ? "%concurrentSequenceValue" : "null")
                << ", ptr " << (call.output ? ownedShapeName(call.output->type) : "null")
                << ", ptr " << (call.output ? "%concurrentSequenceValue" : "null") << ')';
        checkConstructionOperation(sequence, offset);
        if (call.output)
            output_ << "  " << operandName(*call.output) << " = load " << llvmType(call.output->type)
                    << ", ptr %concurrentSequenceValue\n";
    }

    void emitInstruction(const ParallelInvocation& loop, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto prefix = "%parallel" + std::to_string(sequence);
        const auto trampoline = llvmString(loop.worker + ".bridge");
        const auto captureShapes = llvmString(loop.worker + ".captures");
        const auto words = [&](NativeType type) { return valueWords(type, layouts_); };
        const auto pack = [&](const Operand& value, std::string_view address) {
            if (value.type == ScalarType::Boolean) {
                const auto raw = integerField(value, std::string(address) + ".word", offset);
                output_ << "  store i64 " << raw << ", ptr " << address << '\n';
            } else {
                output_ << "  store " << llvmType(value.type) << ' ' << operandName(value)
                        << ", ptr " << address << '\n';
            }
        };

        pack(loop.sequence, "%parallelCaptures");
        std::size_t cursor = words(loop.sequence.type);
        for (std::size_t index = 0; index < loop.captures.size(); ++index) {
            const auto& capture = loop.captures[index];
            const auto slot = prefix + ".capture." + std::to_string(index);
            output_ << "  " << slot << " = getelementptr i64, ptr %parallelCaptures, i64 " << cursor << '\n';
            pack(capture, slot);
            cursor += words(capture.type);
        }

        if (!loop.captures.empty()) {
            textGlobals_ << "@\"" << captureShapes << "\" = private constant [" << loop.captures.size()
                         << " x { ptr, i32 }] [";
            for (std::size_t index = 0; index < loop.captures.size(); ++index) {
                using Kind = native_app_v19::ParallelCaptureKind;
                if (index != 0)
                    textGlobals_ << ", ";
                const auto type = loop.captures[index].type;
                auto kind = Kind::Owned;
                if (type == ScalarType::AtomicReference)
                    kind = Kind::Atomic;
                else if (type == ScalarType::LockReference)
                    kind = Kind::Lock;
                else if (type == ScalarType::Semaphore)
                    kind = Kind::Semaphore;
                else if (type == ScalarType::ConcurrentSequenceReference)
                    kind = Kind::ConcurrentSequence;
                else if (type == ScalarType::ConcurrentDictionaryReference)
                    kind = Kind::ConcurrentDictionary;
                textGlobals_ << "{ ptr, i32 } { ptr " << ownedShapeName(type) << ", i32 "
                             << static_cast<std::int32_t>(kind) << " }";
            }
            textGlobals_ << "]\n";
        }

        textGlobals_ << "define internal i32 @\"" << trampoline << "\"(ptr %runtime, ptr %arguments) {\nentry:\n";
        cursor = 0;
        const auto unpack = [&](NativeType type, std::string_view name) {
            textGlobals_ << "  " << name << ".address = getelementptr i64, ptr %arguments, i64 " << cursor << '\n';
            if (type == ScalarType::Boolean) {
                textGlobals_ << "  " << name << ".word = load i64, ptr " << name << ".address\n"
                             << "  " << name << " = trunc i64 " << name << ".word to i1\n";
            } else {
                textGlobals_ << "  " << name << " = load " << llvmType(type) << ", ptr " << name << ".address\n";
            }
            cursor += words(type);
        };
        unpack(loop.item, "%item");
        for (std::size_t index = 0; index < loop.captures.size(); ++index)
            unpack(loop.captures[index].type, "%capture" + std::to_string(index));
        textGlobals_ << "  %status = call i32 @\"" << llvmString(loop.worker) << "\"(ptr %runtime, ptr null, "
                     << llvmType(loop.item) << " %item";
        for (std::size_t index = 0; index < loop.captures.size(); ++index)
            textGlobals_ << ", " << llvmType(loop.captures[index].type) << " %capture" << index;
        textGlobals_ << ")\n  ret i32 %status\n}\n";

        for (std::size_t index = 0; index < loop.cancellation.size(); ++index) {
            const auto slot = prefix + ".token." + std::to_string(index);
            output_ << "  " << slot << " = getelementptr ptr, ptr %parallelCancellation, i64 " << index << '\n'
                    << "  store ptr " << operandName(loop.cancellation[index]) << ", ptr " << slot << '\n';
        }

        const bool invoking = function_.usesRuntime();
        const auto* operation = "hosted_parallel_owned";
        if (invoking)
            operation = loop.cancellation.empty() ? "parallel_owned" : "parallel_owned_cancellable";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << "(ptr "
                << (invoking ? "%runtime" : "%arena") << ", ptr " << ownedShapeName(loop.sequence.type)
                << ", ptr " << ownedShapeName(loop.item)
                << ", ptr %parallelCaptures, ptr @\"" << trampoline << "\", i64 " << loop.captures.size() << ", ptr ";
        if (loop.captures.empty())
            output_ << "null";
        else
            output_ << "@\"" << captureShapes << '\"';
        if (!loop.cancellation.empty())
            output_ << ", ptr %parallelCancellation, i64 " << loop.cancellation.size();
        else if (!invoking)
            output_ << ", ptr null, i64 0";
        output_ << ')';
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const WaitSemaphore& wait, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "wait_semaphore(ptr %runtime" :
            "hosted_wait_semaphore(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(wait.semaphore) << ", i64 " << operandName(wait.timeout) << ", ptr "
                << (wait.cancellation ? operandName(*wait.cancellation) : "null") << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        if (wait.result) {
            const auto result = registerName(*wait.result);
            output_ << "  " << result << ".word = load i64, ptr %runtimeInteger\n"
                    << "  " << result << " = trunc i64 " << result << ".word to i1\n";
        }
    }

    void emitInstruction(const CreateLock& create, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* destination = function_.usesRuntime()
            ? "pipelang_native_v19_create_lock(ptr %runtime"
            : "pipelang_native_v19_create_hosted_lock(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @" << destination << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << registerName(create.result) << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const AcquireLock& acquire, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "acquire_lock(ptr %runtime" :
            "hosted_acquire_lock(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(acquire.lock) << ')';
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const ReleaseLock&, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime() ? "release_lock(ptr %runtime" :
            "hosted_release_lock(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ")";
        checkConstructionOperation(sequence, offset);
    }

    void emitInstruction(const CreateAtomic& create, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto result = registerName(create.result);
        const auto initial = integerField(create.initial, result + ".initial", offset);
        output_ << "  %status" << sequence << " = call i32 @";
        if (function_.construction() && !function_.usesRuntime())
            output_ << "pipelang_native_v19_create_hosted_atomic(ptr %arena, i32 ";
        else if (create.localSite)
            output_ << "pipelang_native_v19_create_local_atomic(ptr %runtime, i64 " << *create.localSite << ", i32 ";
        else
            output_ << "pipelang_native_v19_create_atomic(ptr %runtime, i32 ";
        output_ << static_cast<std::int32_t>(create.kind) << ", i64 " << initial << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << result << " = load i64, ptr %runtimeInteger\n";
    }

    void emitInstruction(const AtomicCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto prefix = "%atomic" + std::to_string(sequence);
        auto value = std::string("0");
        auto comparand = std::string("0");
        if (!call.arguments.empty())
            value = integerField(call.arguments[0], prefix + ".value", offset);
        if (call.arguments.size() == 2)
            comparand = integerField(call.arguments[1], prefix + ".comparand", offset);
        const auto* operation = function_.usesRuntime() ? "atomic_call(ptr %runtime" :
            "hosted_atomic_call(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                << operandName(call.receiver) << ", i32 " << static_cast<std::int32_t>(call.kind)
                << ", i32 " << static_cast<std::int32_t>(call.operation) << ", i64 " << value
                << ", i64 " << comparand << ", ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        if (!call.result)
            return;

        const auto result = registerName(*call.result);
        if (call.valueType == ScalarType::Boolean) {
            output_ << "  " << result << ".word = load i64, ptr %runtimeInteger\n"
                    << "  " << result << " = trunc i64 " << result << ".word to i1\n";
        } else {
            output_ << "  " << result << " = load i64, ptr %runtimeInteger\n";
        }
    }

    void emitOperation(const ResumedWait& resumed, std::size_t)
    {
        output_ << "  " << registerName(resumed.result) << " = icmp ne i64 %acquired, 0";
    }

    void emitOperation(const ResumedCall& resumed, std::size_t)
    {
        output_ << "  " << registerName(resumed.result) << " = load " << llvmType(resumed.type) << ", ptr %childResult";
    }

    void emitOperation(const Load& load, std::size_t offset)
    {
        if (function_.slots.at(load.slot).type == ScalarType::Boolean) {
            output_ << "  %byte" << load.result.index << " = load i8, ptr %slot" << load.slot;
            location(offset);
            output_ << '\n';
            output_ << "  " << registerName(load.result) << " = trunc i8 %byte" << load.result.index << " to i1";
            return;
        }

        output_ << "  " << registerName(load.result) << " = load "
                << llvmType(function_.slots.at(load.slot).type) << ", ptr %slot" << load.slot;
    }

    void emitOperation(const Store& store, std::size_t offset)
    {
        if (store.declaration)
            declare(store.slot, offset);

        if (store.value.type == ScalarType::Boolean) {
            const auto temporary = booleanStores_++;
            output_ << "  %boolean" << temporary << " = zext i1 " << operandName(store.value) << " to i8";
            location(offset);
            output_ << '\n';
            output_ << "  store i8 %boolean" << temporary << ", ptr %slot" << store.slot;
            return;
        }

        output_ << "  store " << llvmType(store.value.type) << ' ' << operandName(store.value)
                << ", ptr %slot" << store.slot;
    }

    void emitOperation(const Compare& compare, std::size_t)
    {
        if (isFloating(compare.left.type)) {
            const auto result = registerName(compare.result);
            const auto type = compare.left.type;
            decodeFloating(compare.left, result + ".left");
            decodeFloating(compare.right, result + ".right");
            const bool quiet = compare.predicate == Predicate::Equal || compare.predicate == Predicate::NotEqual;
            const auto* floating = llvmFloatingType(type);
            output_ << "  " << result << " = call i1 @llvm.experimental.constrained." << (quiet ? "fcmp" : "fcmps")
                    << '.' << floatingSuffix(type) << '(' << floating << ' ' << result << ".left, "
                    << floating << ' ' << result << ".right, metadata !\"" << llvmFloatingPredicate(compare.predicate)
                    << "\", metadata !\"fpexcept.strict\")";
            return;
        }
        const auto predicate = llvmPredicate(compare.predicate, compare.left.type == ScalarType::UInt32);
        output_ << "  " << registerName(compare.result) << " = icmp " << predicate
                << ' ' << llvmType(compare.left.type) << ' ' << operandName(compare.left)
                << ", " << operandName(compare.right);
    }

    void emitInstruction(const CompareOwned& compare, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        const auto type = llvmType(compare.left.type);
        const auto name = registerName(compare.result);
        const auto* operation = function_.usesRuntime()
            ? "pipelang_native_v19_owned_equal(ptr %runtime"
            : "pipelang_native_v19_hosted_initialization_equal(ptr %arena";
        output_ << "  store " << type << ' ' << operandName(compare.left) << ", ptr %comparisonLeft\n"
                << "  store " << type << ' ' << operandName(compare.right) << ", ptr %comparisonRight\n"
                << "  %status" << sequence << " = call i32 @" << operation << ", ptr "
                << ownedShapeName(compare.left.type) << ", ptr %comparisonLeft, ptr %comparisonRight, ptr %runtimeInteger)";
        checkConstructionOperation(sequence, offset);
        output_ << "  " << name << ".word = load i64, ptr %runtimeInteger\n"
                << "  " << name << " = icmp " << (compare.negate ? "eq" : "ne") << " i64 " << name << ".word, 0";
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const DirectCall& call, std::size_t offset)
    {
        const bool tracksCall = tracksSourceCallDepth_ && function_.usesRuntime() && !call.closesConstruction;
        if (tracksCall) {
            const auto enter = runtimeCalls_++;
            output_ << "  %status" << enter << " = call i32 @pipelang_native_v19_enter_source_call(ptr %runtime)";
            checkRuntimeStatus(enter, offset);
        }
        const bool tracksHostedCall = tracksSourceCallDepth_ && function_.mode == FunctionMode::Construction &&
            !call.closesConstruction;
        if (tracksHostedCall) {
            const auto enter = runtimeCalls_++;
            output_ << "  %status" << enter << " = call i32 @pipelang_native_v19_hosted_enter_source_call(ptr %arena)";
            checkConstructionOperation(enter, offset);
        }
        std::optional<std::size_t> status;
        if (call.abi != CallAbi::Scalar) {
            status = runtimeCalls_++;
            output_ << "  %status" << *status << " = call i32";
        } else if (call.result) {
            output_ << "  " << registerName(*call.result) << " = call " <<
                (call.resultType ? llvmType(*call.resultType) : "i64");
        } else {
            output_ << "  call void";
        }
        output_ << " @\"" << llvmString(call.destination) << "\"(";
        if (call.abi == CallAbi::Construction) {
            if (function_.construction() && !function_.usesRuntime())
                output_ << "ptr %arena, ptr %allocateReceiver";
            else
                output_ << "ptr %runtime, ptr @pipelang_native_v19_allocate_receiver";
            if (call.resultType)
                output_ << ", ptr %childResult";
        } else if (call.abi == CallAbi::Contextual) {
            output_ << "ptr %runtime, ptr " << (call.resultType ? "%childResult" : "null");
        }
        bool first = !status;
        for (const auto& argument : call.arguments) {
            if (!first)
                output_ << ", ";
            first = false;
            output_ << llvmType(argument.type) << ' ' << operandName(argument);
        }
        output_ << ')';
        if (tracksCall) {
            location(offset);
            output_ << "\n  call void @pipelang_native_v19_leave_source_call(ptr %runtime)";
        }
        if (tracksHostedCall) {
            location(offset);
            output_ << "\n  call void @pipelang_native_v19_hosted_leave_source_call(ptr %arena)";
        }
        if (call.abi == CallAbi::Construction) {
            if (function_.usesRuntime()) {
                location(offset);
                if (call.closesConstruction) {
                    output_ << "\n  call void @pipelang_native_v19_finish_construction(ptr %runtime, i32 %status"
                            << *status << ')';
                }
                const auto checked = runtimeCalls_++;
                output_ << "\n  %status" << checked << " = call i32 @pipelang_native_v19_construction_status(ptr %runtime, i32 %status"
                        << *status << ')';
                checkRuntimeStatus(checked, offset);
                return;
            }
            const auto suffix = std::to_string(*status);
            location(offset);
            if (call.closesConstruction) {
                output_ << "\n  call void @pipelang_native_v19_hosted_finish_construction(ptr %arena, i32 %status"
                        << suffix << ')';
            }
            output_ << "\n  %constructed" << suffix << " = icmp eq i32 %status" << suffix << ", 0"
                    << "\n  br i1 %constructed" << suffix << ", label %constructed.next" << suffix
                    << ", label %constructed.failed" << suffix << "\nconstructed.failed" << suffix
                    << ":\n";
            emitHostedFailure("%status" + suffix, currentFaultTarget_);
            output_ << "constructed.next" << suffix << ":\n";
            if (call.resultType) {
                output_ << "  " << registerName(call.result.value()) << " = load " << llvmType(*call.resultType)
                        << ", ptr %childResult";
                location(offset);
                output_ << '\n';
            }
            return;
        }
        if (status) {
            checkRuntimeStatus(*status, offset);
            if (!call.resultType)
                return;
            output_ << "  " << registerName(call.result.value()) << " = load " << llvmType(*call.resultType)
                    << ", ptr %childResult";
        }
        location(offset);
        output_ << '\n';
    }

    void emitInstruction(const AllocateObject& object, std::size_t offset)
    {
        const auto name = registerName(object.result);
        if (function_.construction() && !function_.usesRuntime()) {
            const auto begin = runtimeCalls_++;
            const auto* operation = tracksSourceCallDepth_ ? "hosted_begin_tracked_construction" :
                "hosted_begin_construction";
            output_ << "  %status" << begin << " = call i32 @pipelang_native_v19_" << operation << "(ptr %arena)";
            checkConstructionOperation(begin, offset);
            output_ << "  " << name << " = call ptr %allocateReceiver(ptr %arena, i64 " << object.words
                    << ", i64 " << object.tag << ')';
            location(offset);
            output_ << "\n  " << name << ".allocated = icmp ne ptr " << name << ", null\n"
                    << "  br i1 " << name << ".allocated, label %receiver.allocated" << object.result.index
                    << ", label %receiver.exhausted" << object.result.index
                    << "\nreceiver.exhausted" << object.result.index << ":\n"
                    << "  call void @pipelang_native_v19_hosted_finish_construction(ptr %arena, i32 1)\n"
                    << "  br label %" << sourceGuardLabel("receiver_allocation_failed", offset)
                    << "\nreceiver.allocated" << object.result.index << ":\n";
            return;
        }
        if (function_.usesRuntime()) {
            const auto begin = runtimeCalls_++;
            output_ << "  %status" << begin << " = call i32 @pipelang_native_v19_begin_construction(ptr %runtime, i32 "
                    << (tracksSourceCallDepth_ ? 1 : 0) << ')';
            checkRuntimeStatus(begin, offset);
        }
        if (object.arena) {
            const auto* allocate = retainsObjectGraphs_ && object.graphEligible
                ? "pipelang_native_v19_allocate_constructed_graph_receiver"
                : "pipelang_native_v19_allocate_constructed_receiver";
            output_ << "  " << name << " = call ptr @" << allocate << "(ptr %runtime, i64 "
                    << object.words << ", i64 " << object.tag << ')';
            location(offset);
            const auto status = runtimeCalls_++;
            output_ << "\n  " << name << ".failed = icmp eq ptr " << name << ", null\n"
                    << "  " << name << ".status = zext i1 " << name << ".failed to i32\n"
                    << "  %status" << status << " = call i32 @pipelang_native_v19_construction_status(ptr %runtime, i32 "
                    << name << ".status)";
            checkRuntimeStatus(status, offset);
            return;
        }
        const auto layout = "[" + std::to_string(native_app_v19::receiverHeaderWords + object.words) + " x i64]";
        output_ << "  " << name;
        if (function_.continuation()) {
            const auto words = slotStorageWords(function_, layouts_) + object.frameOffset;
            output_ << " = getelementptr i64, ptr %storage, i64 " << words;
        } else {
            output_ << " = getelementptr i64, ptr %stackReceivers, i64 " << object.frameOffset;
        }
        location(offset);
        output_ << "\n  store " << layout << " zeroinitializer, ptr " << name << ", align 8";
        location(offset);
        output_ << "\n  store i64 " << object.words << ", ptr " << name << ", align 8";
        location(offset);
        output_ << '\n';
    }

    void emitOperation(const ReadReceiverTag& read, std::size_t offset)
    {
        const auto result = registerName(read.result);
        const auto receiver = operandName(read.receiver);
        const auto next = "interface.initialized" + std::to_string(read.result.index);
        output_ << "  " << result << ".valid = icmp ne ptr " << receiver << ", null\n"
                << "  br i1 " << result << ".valid, label %" << next << ", label %"
                << sourceGuardLabel("uninitialized_receiver", offset);
        location(offset);
        output_ << '\n' << next << ":\n"
                << "  " << result << ".address = getelementptr i64, ptr " << receiver << ", i64 "
                << native_app_v19::receiverTagWord << '\n'
                << "  " << result << " = load i64, ptr " << result << ".address";
    }

    void emitInstruction(const RequireReceiver& read, std::size_t offset)
    {
        const auto id = std::to_string(runtimeCalls_++);
        const auto next = "class.initialized" + id;
        output_ << "  %class.valid" << id << " = icmp ne ptr " << operandName(read.receiver) << ", null\n"
                << "  br i1 %class.valid" << id << ", label %" << next << ", label %"
                << sourceGuardLabel("uninitialized_receiver", offset);
        location(offset);
        output_ << '\n' << next << ":\n";
    }

    std::string hostString(std::string_view name, const std::string& value, std::size_t sequence)
    {
        auto symbol = "@.pipe.host." + std::string(name) + '.' + std::to_string(metadataStart_) + '.' +
            std::to_string(sequence);
        textGlobals_ << symbol << " = private unnamed_addr constant [" << value.size() + 1
                     << " x i8] c\"" << llvmString(value) << "\\00\"\n";
        return symbol;
    }

    void emitInstruction(const HostCall& call, std::size_t offset)
    {
        const auto sequence = runtimeCalls_++;
        std::size_t word = 0;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            const auto& argument = call.arguments[index];
            const auto base = "%host" + std::to_string(sequence) + ".argument" + std::to_string(index);
            const auto value = integerField(argument, base + ".integer", offset);
            const auto shape = argument.type == ScalarType::CancellationToken ? "null" : ownedShapeName(argument.type);
            output_ << "  " << base << ".payload = getelementptr i64, ptr %hostPayload, i64 " << word
                    << "\n  store " << fieldStorageType(argument.type) << ' ' << value << ", ptr " << base << ".payload\n"
                    << "  " << base << ".shape = getelementptr %pipe.host.argument, ptr %hostArguments, i64 " << index
                    << ", i32 0\n  store ptr " << shape << ", ptr " << base << ".shape\n"
                    << "  " << base << ".value = getelementptr %pipe.host.argument, ptr %hostArguments, i64 " << index
                    << ", i32 1\n  store ptr " << base << ".payload, ptr " << base << ".value\n";
            word += valueWords(argument.type, layouts_);
        }
        const auto interfaceName = hostString("interface", call.interfaceName, sequence);
        const auto signature = hostString("signature", call.signature, sequence);
        const auto* runtimeCall = call.reusableByteSlot ? "host_call_with_reusable_bytes" :
            (call.immediateResult ? "host_call" : "prepare_host_call");
        output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_"
                << (function_.usesRuntime() ? runtimeCall : "hosted_initialization_call")
                << (function_.usesRuntime() ? "(ptr %runtime, i64 " : "(ptr %arena, i64 ")
                << operandName(call.service) << ", ptr " << interfaceName << ", ptr " << signature << ", ptr "
                << (call.arguments.empty() ? "null" : "%hostArguments") << ", i64 " << call.arguments.size()
                << ", ptr " << (call.response ? ownedShapeName(*call.response) : "null");
        if (call.immediateResult)
            output_ << ", i64 " << (call.wrapsHostStatus ? 1 : 0) << ", ptr %hostResult";
        if (call.reusableByteSlot)
            output_ << ", ptr " << (function_.continuation() && debug_ ? "%savedSlot" : "%slot")
                    << *call.reusableByteSlot;
        output_ << ')';
        checkConstructionOperation(sequence, offset);
        if (call.immediateResult) {
            output_ << "  " << registerName(*call.immediateResult) << " = load " << llvmType(call.resultType) << ", ptr %hostResult";
            location(offset);
            output_ << '\n';
        }
    }

    std::string fieldAddress(const Operand& receiver, std::size_t field, std::size_t offset)
    {
        const auto address = "%field" + std::to_string(fields_++);
        output_ << "  " << address << " = getelementptr i64, ptr " << operandName(receiver) << ", i64 "
                << native_app_v19::receiverHeaderWords + field;
        location(offset);
        output_ << '\n';
        return address;
    }

    void emitFieldLoad(const ReadField& read, std::size_t offset)
    {
        const auto result = registerName(read.result);
        if ((function_.usesRuntime() || function_.construction()) && read.type == ScalarType::AtomicReference) {
            const auto status = runtimeCalls_++;
            const auto kind = atomicKind(layouts_[read.type.layout].fields.front().type);
            const auto* operation = function_.usesRuntime() ? "read_receiver_atomic(ptr %runtime" :
                "hosted_read_receiver_atomic(ptr %arena";
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", i32 " << static_cast<std::int32_t>(kind)
                    << ", ptr %runtimeInteger)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }
        if ((function_.usesRuntime() || function_.construction()) && read.type == ScalarType::ConcurrentDictionaryReference) {
            const auto status = runtimeCalls_++;
            const auto* operation = function_.usesRuntime() ? "read_receiver_concurrent_dictionary(ptr %runtime" :
                "hosted_read_receiver_concurrent_dictionary(ptr %arena";
            const auto& parts = layouts_[read.type.layout].fields;
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", ptr "
                    << ownedShapeName(parts[0].type) << ", ptr " << ownedShapeName(parts[1].type) << ", ptr %runtimeInteger)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }
        if ((function_.usesRuntime() || function_.construction()) && read.type == ScalarType::ConcurrentSequenceReference) {
            const auto status = runtimeCalls_++;
            const auto* operation = function_.usesRuntime() ? "read_receiver_concurrent_sequence(ptr %runtime" :
                "hosted_read_receiver_concurrent_sequence(ptr %arena";
            const auto element = layouts_[read.type.layout].fields.front().type;
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", i32 "
                    << static_cast<std::int32_t>(concurrentSequenceKind(read.type)) << ", ptr " << ownedShapeName(element)
                    << ", ptr %runtimeInteger)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }
        if ((function_.usesRuntime() || function_.construction()) && read.type == ScalarType::LockReference) {
            const auto status = runtimeCalls_++;
            const auto* operation = function_.usesRuntime() ? "read_receiver_lock(ptr %runtime" :
                "hosted_read_receiver_lock(ptr %arena";
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", ptr %runtimeInteger)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }
        if ((function_.usesRuntime() || function_.construction()) && read.type == ScalarType::Semaphore) {
            const auto status = runtimeCalls_++;
            const auto* operation = function_.usesRuntime() ? "read_receiver_semaphore(ptr %runtime" :
                "hosted_read_receiver_semaphore(ptr %arena";
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", ptr %runtimeInteger)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load i64, ptr %runtimeInteger";
            return;
        }
        const bool ownedValues = containsReceiverValues(read.type, layouts_);
        const bool ownedSets = containsReceiverSets(read.type, layouts_);
        if (function_.construction() && retainsScopedBytes_ && (ownedValues || ownedSets)) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status
                    << " = call i32 @pipelang_native_v19_hosted_read_receiver_collections(ptr %arena, ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", ptr "
                    << ownedShapeName(read.type) << ", ptr %receiverRecord)";
            checkConstructionOperation(status, offset);
            output_ << "  " << result << " = load " << llvmType(read.type) << ", ptr %receiverRecord";
            return;
        }
        if (function_.usesRuntime() && (ownedValues || ownedSets)) {
            const auto status = runtimeCalls_++;
            const auto storage = ownedValues ? "values" : "sets";
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_read_receiver_" << storage
                    << "(ptr %runtime, ptr "
                    << operandName(read.receiver) << ", i64 " << read.field << ", ptr "
                    << ownedShapeName(read.type) << ", ptr %receiverRecord)";
            checkRuntimeStatus(status, offset);
            output_ << "  " << result << " = load " << llvmType(read.type) << ", ptr %receiverRecord";
            return;
        }
        const auto address = fieldAddress(read.receiver, read.field, offset);
        if (function_.usesRuntime() && allowsMultipleTasks_ &&
            (containsInlineField(read.type, ScalarType::Text) || containsInlineField(read.type, ScalarType::ByteBuffer))) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_borrow_receiver_bytes(ptr %runtime, ptr "
                    << ownedShapeName(read.type) << ", ptr " << address << ')';
            checkRuntimeStatus(status, offset);
        }
        if (read.type == ScalarType::Boolean) {
            const auto byte = "%fieldByte" + std::to_string(read.result.index);
            output_ << "  " << byte << " = load i8, ptr " << address;
            location(offset);
            output_ << "\n  " << result << " = trunc i8 " << byte << " to i1";
        } else if (read.type == ScalarType::Receiver || read.type == ScalarType::ContextOwnerReference) {
            const auto* diagnostic = read.type == ScalarType::Receiver
                ? "uninitialized_receiver" : "uninitialized_context_owner";
            output_ << "  " << result << ".word = load i64, ptr " << address;
            location(offset);
            output_ << "\n  " << result << ".valid = icmp ne i64 " << result << ".word, 0\n"
                    << "  br i1 " << result << ".valid, label %receiver.initialized" << read.result.index
                    << ", label %" << sourceGuardLabel(diagnostic, offset)
                    << "\nreceiver.initialized" << read.result.index << ":\n";
            if (read.type == ScalarType::Receiver)
                output_ << "  " << result << " = inttoptr i64 " << result << ".word to ptr";
            else
                output_ << "  " << result << " = add i64 " << result << ".word, 0";
        } else {
            output_ << "  " << result << " = load " << llvmType(read.type) << ", ptr " << address;
        }
    }

    void emitInstruction(const ReadField& read, std::size_t offset)
    {
        emitFieldLoad(read, offset);
        location(offset);
        output_ << '\n';
        const bool sourceReference = read.type == ScalarType::Receiver || read.type == ScalarType::ClassReference ||
            read.type == ScalarType::Interface;
        if (allowsMultipleTasks_ && function_.usesRuntime() && sourceReference) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status
                    << " = call i32 @pipelang_native_v19_pin_receiver(ptr %runtime, ptr " << registerName(read.result) << ')';
            checkRuntimeStatus(status, offset);
        }
    }

    void emitInstruction(const WriteField& write, std::size_t offset)
    {
        const bool journal = function_.usesRuntime() && (!write.initializing || retainsObjectGraphs_);
        const auto synchronization = receiverSynchronizationKind(write.value.type);
        if (journal && retainsObjectGraphs_ && synchronization) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status
                    << " = call i32 @pipelang_native_v19_write_receiver_synchronization(ptr %runtime, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", i32 "
                    << static_cast<std::int32_t>(*synchronization) << ", i64 " << operandName(write.value) << ')';
            checkRuntimeStatus(status, offset);
            return;
        }
        if (journal && retainsObjectGraphs_ &&
            (write.value.type == ScalarType::Receiver || write.value.type == ScalarType::ClassReference ||
             write.value.type == ScalarType::Interface)) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status
                    << " = call i32 @pipelang_native_v19_write_receiver_reference(ptr %runtime, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", ptr "
                    << operandName(write.value) << ')';
            checkRuntimeStatus(status, offset);
            return;
        }
        const bool ownedValues = containsReceiverValues(write.value.type, layouts_);
        const bool ownedSets = containsReceiverSets(write.value.type, layouts_);
        if (function_.construction() && retainsScopedBytes_ && (ownedValues || ownedSets)) {
            const auto status = runtimeCalls_++;
            output_ << "  store " << llvmType(write.value.type) << ' ' << operandName(write.value)
                    << ", ptr %receiverRecord\n"
                    << "  %status" << status
                    << " = call i32 @pipelang_native_v19_hosted_write_receiver_collections(ptr %arena, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", ptr "
                    << ownedShapeName(write.value.type) << ", ptr %receiverRecord)";
            checkConstructionOperation(status, offset);
            return;
        }
        if (journal && (write.value.type == ScalarType::Record || write.value.type == ScalarType::HashSet || ownedValues)) {
            const auto status = runtimeCalls_++;
            const auto* storage = "record";
            if (ownedValues)
                storage = "values";
            else if (ownedSets)
                storage = "sets";
            output_ << "  store " << llvmType(write.value.type) << ' ' << operandName(write.value)
                    << ", ptr %receiverRecord\n"
                    << "  %status" << status << " = call i32 @pipelang_native_v19_write_receiver_" << storage << "(ptr %runtime, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", ptr "
                    << ownedShapeName(write.value.type) << ", ptr %receiverRecord)";
            checkRuntimeStatus(status, offset);
            return;
        }
        if (journal && write.value.type == ScalarType::ByteBuffer) {
            const auto status = runtimeCalls_++;
            output_ << "  store %pipe.bytes " << operandName(write.value) << ", ptr %byteField\n"
                    << "  %status" << status << " = call i32 @pipelang_native_v19_write_receiver_bytes(ptr %runtime, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", ptr %byteField)";
            checkRuntimeStatus(status, offset);
            return;
        }
        if (journal && write.value.type == ScalarType::Text) {
            const auto status = runtimeCalls_++;
            const auto name = "%receiverText" + std::to_string(status);
            const auto value = operandName(write.value);
            output_ << "  " << name << ".bytes = extractvalue %pipe.text " << value << ", 0\n"
                    << "  " << name << ".size = extractvalue %pipe.text " << value << ", 1\n"
                    << "  %status" << status << " = call i32 @pipelang_native_v19_write_receiver_text(ptr %runtime, ptr "
                    << operandName(write.receiver) << ", i64 " << write.field << ", ptr " << name << ".bytes, i64 "
                    << name << ".size)";
            checkRuntimeStatus(status, offset);
            return;
        }
        if (journal) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_track_receiver(ptr %runtime, ptr "
                    << operandName(write.receiver) << ')';
            checkRuntimeStatus(status, offset);
        }
        const auto address = fieldAddress(write.receiver, write.field, offset);
        if (function_.mode == FunctionMode::Construction) {
            const auto status = runtimeCalls_++;
            output_ << "  %status" << status << " = call i32 @pipelang_native_v19_hosted_track_receiver(ptr %arena, ptr "
                    << operandName(write.receiver) << ')';
            checkConstructionOperation(status, offset);
        }
        if (usesAggregateLayout(write.value.type) || write.value.type == ScalarType::Text ||
            write.value.type == ScalarType::ByteBuffer) {
            output_ << "  store " << llvmType(write.value.type) << ' ' << operandName(write.value) << ", ptr " << address;
        } else {
            std::string value;
            if (write.value.type == ScalarType::Receiver || write.value.type == ScalarType::ClassReference ||
                write.value.type == ScalarType::Interface) {
                value = address + ".word";
                output_ << "  " << value << " = ptrtoint ptr " << operandName(write.value) << " to i64\n";
            } else {
                value = integerArgument(write.value, offset);
            }
            output_ << "  store i64 " << value << ", ptr " << address;
        }
        location(offset);
        output_ << '\n';
        if (function_.mode == FunctionMode::Construction) {
            const bool containsBuffer = containsInlineField(write.value.type, ScalarType::ByteBuffer);
            if (retainsScopedBytes_ && (containsBuffer || containsInlineField(write.value.type, ScalarType::Text))) {
                const auto status = runtimeCalls_++;
                output_ << "  %status" << status << " = call i32 @pipelang_native_v19_hosted_retain_receiver_bytes(ptr %arena, ptr "
                        << operandName(write.receiver) << ", ptr " << ownedShapeName(write.value.type)
                        << ", ptr " << address << ')';
                checkConstructionOperation(status, offset);
            } else if (containsBuffer) {
                const auto status = runtimeCalls_++;
                output_ << "  %status" << status << " = call i32 @pipelang_native_v19_hosted_record_byte_fields(ptr %arena, ptr "
                        << ownedShapeName(write.value.type) << ", ptr " << address << ')';
                checkConstructionOperation(status, offset);
            }
            if (retainsObjectGraphs_)
                recordConstructedField(write, address, synchronization, offset);
        }
    }

    bool containsInlineField(NativeType type, ScalarType leaf) const
    {
        if (type == leaf)
            return true;
        if (type != ScalarType::Record)
            return false;
        return std::any_of(layouts_[type.layout].fields.begin(), layouts_[type.layout].fields.end(),
                           [&](const auto& field) { return containsInlineField(field.type, leaf); });
    }

    void recordConstructedField(const WriteField& write, const std::string& address,
                                std::optional<native_app_v19::ReceiverSynchronizationKind> synchronization,
                                std::size_t offset)
    {
        const auto type = write.value.type;
        const bool reference = containsSourceReferences(type, layouts_);
        if (!reference && !synchronization)
            return;

        const bool shaped = reference && usesAggregateLayout(type);
        const auto status = runtimeCalls_++;
        output_ << "  %status" << status
                << " = call i32 @pipelang_native_v19_record_constructed_receiver_field(ptr %arena, ptr "
                << operandName(write.receiver) << ", ptr " << address << ", ptr "
                << (shaped ? ownedShapeName(type) : "null") << ", i32 "
                << (synchronization ? static_cast<std::int32_t>(*synchronization) : -1) << ')';
        checkConstructionOperation(status, offset);
    }

    void emitOperation(const TextLiteral& literal, std::size_t offset)
    {
        const auto name = registerName(literal.result);
        if (literal.bytes.empty()) {
            output_ << "  " << name << " = insertvalue %pipe.text zeroinitializer, i64 0, 1";
            return;
        }

        const auto global = "@.pipe.text." + std::to_string(metadataStart_) + "." + std::to_string(literal.result.index);
        textGlobals_ << global << " = private unnamed_addr constant [" << literal.bytes.size()
                     << " x i8] c\"" << llvmString(literal.bytes) << "\"\n";
        output_ << "  " << name << ".ptr = insertvalue %pipe.text zeroinitializer, ptr " << global << ", 0";
        location(offset);
        output_ << "\n  " << name << " = insertvalue %pipe.text " << name << ".ptr, i64 " << literal.bytes.size() << ", 1";
    }

    static native_app_v19::TextOperation runtimeTextOperation(TextOperation operation)
    {
        using Runtime = native_app_v19::TextOperation;
        switch (operation) {
        case TextOperation::Concatenate: return Runtime::Concatenate;
        case TextOperation::ByteLengthIs: return Runtime::ByteLengthIs;
        case TextOperation::ByteAtOr: return Runtime::ByteAtOr;
        case TextOperation::StartsWith: return Runtime::StartsWith;
        case TextOperation::FromInt: return Runtime::FromInt;
        case TextOperation::Utf8Length: return Runtime::Utf8Length;
        case TextOperation::Utf8Slice: return Runtime::Utf8Slice;
        case TextOperation::Utf8IndexOf: return Runtime::Utf8IndexOf;
        }
        throw std::logic_error("unknown native text operation");
    }

    void emitInstruction(const TextCall& call, std::size_t offset)
    {
        std::array<std::string, 2> texts{"null", "null"};
        std::array<std::string, 2> integers{"0", "0"};
        std::size_t textCount = 0;
        std::size_t integerCount = 0;
        for (const auto& argument : call.arguments) {
            if (argument.type == ScalarType::Text) {
                auto& storage = texts.at(textCount++);
                storage = "%textOperand" + std::to_string(textCount - 1);
                output_ << "  store %pipe.text " << operandName(argument) << ", ptr " << storage << '\n';
            } else {
                integers.at(integerCount++) = operandName(argument);
            }
        }
        const auto sequence = runtimeCalls_++;
        const auto* operation = function_.usesRuntime()
            ? "pipelang_native_v19_text_operation(ptr %runtime"
            : "pipelang_native_v19_hosted_initialization_text(ptr %arena";
        output_ << "  %status" << sequence << " = call i32 @" << operation << ", i32 "
                << static_cast<std::int32_t>(runtimeTextOperation(call.operation)) << ", ptr " << texts[0] << ", ptr "
                << texts[1] << ", i64 " << integers[0] << ", i64 " << integers[1] << ", ptr %textOutput)";
        checkConstructionOperation(sequence, offset);
        const auto name = registerName(call.result);
        if (call.resultType == ScalarType::Boolean) {
            output_ << "  " << name << ".word = load i64, ptr %textOutput\n"
                    << "  " << name << " = trunc i64 " << name << ".word to i1";
        } else {
            output_ << "  " << name << " = load " << llvmType(call.resultType) << ", ptr %textOutput";
        }
        location(offset);
        output_ << '\n';
    }

    void emitOperation(const ByteLength& length, std::size_t)
    {
        output_ << "  " << registerName(length.result) << " = extractvalue %pipe.bytes "
                << operandName(length.receiver) << ", 2";
    }

    void emitInstruction(const ByteCall& call, std::size_t offset)
    {
        const auto& receiver = call.arguments.at(0);
        output_ << "  store " << llvmType(receiver.type) << ' ' << operandName(receiver) << ", ptr %byteOperand0\n";
        const char* argument = "null";
        std::array<std::string, 2> integers{"0", "0"};
        std::size_t integerCount = 0;
        for (const auto& input : std::span(call.arguments).subspan(1)) {
            if (input.type == ScalarType::ByteBuffer) {
                argument = "%byteOperand1";
                output_ << "  store %pipe.bytes " << operandName(input) << ", ptr " << argument << '\n';
            } else {
                integers.at(integerCount++) = operandName(input);
            }
        }
        const auto status = runtimeCalls_++;
        const auto* operation = function_.usesRuntime()
            ? "pipelang_native_v19_bytes(ptr %runtime" : "pipelang_native_v19_hosted_initialization_bytes(ptr %arena";
        output_ << "  %status" << status << " = call i32 @" << operation << ", i32 "
                << static_cast<std::int32_t>(call.operation) << ", ptr %byteOperand0, ptr " << argument
                << ", i64 " << integers[0] << ", i64 " << integers[1] << ", ptr %byteOutput)";
        checkConstructionOperation(status, offset);
        output_ << "  " << registerName(call.result) << " = load " << llvmType(call.resultType) << ", ptr %byteOutput";
        location(offset);
        output_ << '\n';
    }

    static std::int64_t resultTag(ResultKind kind)
    {
        using native_app_v19::ResultTag;
        switch (kind) {
        case ResultKind::Success: return static_cast<std::int64_t>(ResultTag::Success);
        case ResultKind::Failure: return static_cast<std::int64_t>(ResultTag::Failure);
        case ResultKind::Cancelled: return static_cast<std::int64_t>(ResultTag::Cancelled);
        }
        throw std::logic_error("unrecognized native result outcome");
    }

    void emitOperation(const ConstructResult& result, std::size_t offset)
    {
        const auto name = registerName(result.result);
        const auto type = llvmType(result.type);
        const auto hasError = !std::holds_alternative<ZeroValue>(result.error.value);
        const auto tag = hasError || result.value ? name + ".tag" : name;
        output_ << "  " << tag << " = insertvalue " << type << " zeroinitializer, i64 " << resultTag(result.kind) << ", 0";
        auto current = tag;
        if (hasError) {
            location(offset);
            output_ << '\n';
            const auto error = integerField(result.error, name + ".error.word", offset);
            const auto next = result.value ? name + ".error" : name;
            output_ << "  " << next << " = insertvalue " << type << ' ' << current << ", "
                    << fieldStorageType(result.error.type) << ' ' << error << ", 1";
            current = next;
        }
        if (result.value) {
            location(offset);
            output_ << '\n';
            const auto payload = integerField(*result.value, name + ".payload", offset);
            output_ << "  " << name << " = insertvalue " << type << ' ' << current << ", "
                    << fieldStorageType(result.value->type) << ' ' << payload << ", 2";
        }
    }

    void emitOperation(const EraseResult& erased, std::size_t offset)
    {
        const auto status = runtimeCalls_++;
        const bool runtime = function_.usesRuntime();
        output_ << "  store " << llvmType(erased.source.type) << ' ' << operandName(erased.source)
                << ", ptr %resultInterfaceInput\n"
                << "  %status" << status << " = call i32 "
                << (runtime ? "@pipelang_native_v19_erase_result(ptr %runtime, ptr "
                            : "@pipelang_native_v19_erase_hosted_result(ptr %arena, ptr ")
                << ownedShapeName(erased.source.type)
                << ", ptr %resultInterfaceInput, ptr %resultInterfaceOutput)";
        checkConstructionOperation(status, offset);
        output_ << "  " << registerName(erased.result) << " = load " << llvmType(erased.erased)
                << ", ptr %resultInterfaceOutput";
    }

    void emitInstruction(const ValidateConstructedValue& check, std::size_t offset)
    {
        const auto status = runtimeCalls_++;
        const bool runtime = function_.usesRuntime();
        output_ << "  store " << llvmType(check.value.type) << ' ' << operandName(check.value)
                << ", ptr %resultInterfaceInput\n"
                << "  %status" << status << " = call i32 "
                << (runtime ? "@pipelang_native_v19_validate_constructed_value(ptr %runtime, ptr "
                            : "@pipelang_native_v19_validate_hosted_value(ptr %arena, ptr ")
                << ownedShapeName(check.value.type) << ", ptr %resultInterfaceInput)";
        checkConstructionOperation(status, offset);
    }

    void emitOperation(const ReadResult& result, std::size_t offset)
    {
        const auto name = registerName(result.result);
        const auto receiver = operandName(result.receiver);
        const auto type = llvmType(result.receiver.type);
        const bool erased = result.receiver.type == ScalarType::ResultInterface;
        if (erased) {
            const auto status = runtimeCalls_++;
            const auto* operation = function_.usesRuntime()
                ? "require_result_interface(ptr %runtime"
                : "hosted_require_result_interface(ptr %arena";
            output_ << "  " << name << ".identity = extractvalue " << type << ' ' << receiver << ", 0\n"
                    << "  %status" << status << " = call i32 @pipelang_native_v19_" << operation << ", i64 "
                    << name << ".identity)";
            checkConstructionOperation(status, offset);
        }
        if (result.property == ResultProperty::Error) {
            const auto field = erased ? 4U : 1U;
            extractField(name, result.receiver, field, layouts_[result.receiver.type.layout].fields[field].type, offset);
            return;
        }
        if (result.property == ResultProperty::SuccessPayload) {
            extractField(name, result.receiver, 2, layouts_[result.receiver.type.layout].fields[2].type, offset);
            return;
        }

        output_ << "  " << name << ".tag = extractvalue " << type << ' ' << receiver << ", " << (erased ? 1 : 0);
        location(offset);
        output_ << '\n';
        auto desired = ResultKind::Success;
        if (result.property == ResultProperty::IsFailure)
            desired = ResultKind::Failure;
        else if (result.property == ResultProperty::IsCancelled)
            desired = ResultKind::Cancelled;

        if (result.property != ResultProperty::Value) {
            output_ << "  " << name << " = icmp eq i64 " << name << ".tag, " << resultTag(desired);
            return;
        }

        if (!function_.usesRuntime() && currentFaultTarget_ == 0)
            hasHostedResultGuard_ = true;
        output_ << "  " << name << ".valid = icmp eq i64 " << name << ".tag, " << resultTag(ResultKind::Success);
        location(offset);
        const auto next = "result_value_" + std::to_string(result.result.index);
        output_ << "\n  br i1 " << name << ".valid, label %" << next << ", label %"
                << sourceGuardLabel("invalid_result_value", offset);
        location(offset);
        output_ << '\n' << next << ":\n";
        extractField(name, result.receiver, 2, layouts_[result.receiver.type.layout].fields[2].type, offset);
    }

    void emitOperation(const ConstructOptional& optional, std::size_t offset)
    {
        const auto name = registerName(optional.result);
        const auto type = llvmType(optional.type);
        if (!optional.value) {
            output_ << "  " << name << " = insertvalue " << type << " zeroinitializer, i64 0, 0";
            return;
        }
        const auto& value = *optional.value;
        const auto payload = integerField(value, name + ".payload", offset);
        output_ << "  " << name << ".tag = insertvalue " << type << " zeroinitializer, i64 1, 0";
        location(offset);
        output_ << "\n  " << name << " = insertvalue " << type << ' ' << name << ".tag, "
                << fieldStorageType(value.type) << ' ' << payload << ", 1";
    }

    void emitOperation(const ReadOptional& optional, std::size_t offset)
    {
        const auto name = registerName(optional.result);
        output_ << "  " << name << ".tag = extractvalue " << llvmType(optional.receiver.type)
                << ' ' << operandName(optional.receiver) << ", 0";
        location(offset);
        if (!optional.projectValue) {
            output_ << "\n  " << name << " = icmp ne i64 " << name << ".tag, 0";
            return;
        }

        const auto next = "optional_value_" + std::to_string(optional.result.index);
        output_ << "\n  " << name << ".valid = icmp ne i64 " << name << ".tag, 0";
        location(offset);
        output_ << "\n  br i1 " << name << ".valid, label %" << next << ", label %"
                << sourceGuardLabel("invalid_optional_value", offset);
        location(offset);
        output_ << '\n' << next << ":\n";
        extractField(name, optional.receiver, 1, layouts_[optional.receiver.type.layout].fields[1].type, offset);
    }

    std::string integerField(const Operand& field, const std::string& name, std::size_t offset)
    {
        if (field.type != ScalarType::Boolean)
            return operandName(field);
        output_ << "  " << name << " = zext i1 " << operandName(field) << " to i64";
        location(offset);
        output_ << '\n';
        return name;
    }

    void extractField(const std::string& name, const Operand& record, std::size_t index,
                      NativeType type, std::size_t offset)
    {
        output_ << "  " << name;
        if (type == ScalarType::Boolean)
            output_ << ".word";
        output_ << " = extractvalue " << llvmType(record.type) << ' ' << operandName(record) << ", " << index;
        if (type == ScalarType::Boolean) {
            location(offset);
            output_ << "\n  " << name << " = trunc i64 " << name << ".word to i1";
        }
    }

    void emitOperation(const ConstructRecord& record, std::size_t offset)
    {
        const auto name = registerName(record.result);
        const auto type = llvmType(record.type);
        if (record.fields.empty()) {
            output_ << "  " << name << " = insertvalue " << type << " zeroinitializer, i64 0, 0";
            return;
        }

        std::string current = "zeroinitializer";
        for (std::size_t index = 0; index < record.fields.size(); ++index) {
            const auto& field = record.fields[index];
            const auto indexName = std::to_string(index);
            const auto stored = integerField(field, name + ".boolean" + indexName, offset);
            const bool last = index + 1 == record.fields.size();
            const auto next = last ? name : name + ".field" + indexName;
            output_ << "  " << next << " = insertvalue " << type << ' ' << current << ", "
                    << fieldStorageType(field.type) << ' ' << stored << ", " << index;
            if (!last) {
                location(offset);
                output_ << '\n';
            }
            current = next;
        }
    }

    void emitOperation(const ReadRecordField& field, std::size_t offset)
    {
        extractField(registerName(field.result), field.record, field.field, field.type, offset);
    }

    void emitOperation(const UpdateRecordField& field, std::size_t offset)
    {
        const auto result = registerName(field.result);
        const auto payload = integerField(field.value, result + ".field", offset);
        output_ << "  " << result << " = insertvalue " << llvmType(field.record.type) << ' '
                << operandName(field.record) << ", " << fieldStorageType(field.value.type) << ' ' << payload;
        for (const auto index : field.path)
            output_ << ", " << index;
    }

    void emitExit(const Jump& jump, std::size_t)
    {
        output_ << "  br label %block" << jump.destination << '\n';
    }

    void emitExit(const Branch& branch, std::size_t offset)
    {
        output_ << "  br i1 " << operandName(branch.condition)
                << ", label %block" << branch.whenTrue << ", label %block" << branch.whenFalse;
        location(offset);
        output_ << '\n';
    }

    void emitExit(const Return& returned, std::size_t offset)
    {
        if (function_.usesRuntime() || function_.construction()) {
            if (returned.value) {
                const auto& value = *returned.value;
                output_ << "  store " << llvmType(value.type) << ' ' << operandName(value) << ", ptr %resultOutput";
                location(offset);
                output_ << '\n';
            }
            if (function_.continuation())
                output_ << "  ret void";
            else
                output_ << "  ret i32 0";
        } else if (returned.value) {
            const auto type = returned.value->type;
            output_ << "  ret " << (type == ScalarType::Record ? llvmType(type) : "i64")
                    << ' ' << operandName(*returned.value);
        } else {
            output_ << "  ret void";
        }
        location(offset);
        output_ << '\n';
    }

    void emitExit(const Unreachable&, std::size_t)
    {
        output_ << "  unreachable\n";
    }

    void emitExit(const InvalidDispatch&, std::size_t offset)
    {
        if (!function_.usesRuntime()) {
            const auto sequence = runtimeCalls_++;
            output_ << "  %status" << sequence << " = call i32 @pipelang_native_v19_hosted_invalid_dispatch(ptr %arena)";
            location(offset);
            output_ << '\n';
            emitHostedFailure("%status" + std::to_string(sequence), currentFaultTarget_);
            return;
        }
        output_ << "  call void @pipelang_native_v19_invalid_dispatch(ptr %runtime)";
        location(offset);
        output_ << '\n';
        emitSourceNote("failure", offset);
        output_ << "  br label %runtime_failure\n";
    }

    void emitExit(const DispatchReceiver& dispatch, std::size_t offset)
    {
        output_ << "  switch i64 " << operandName(dispatch.tag) << ", label %block" << dispatch.fallback << " [\n";
        for (const auto& [tag, block] : dispatch.branches)
            output_ << "    i64 " << tag << ", label %block" << block << '\n';
        output_ << "  ]";
        location(offset);
        output_ << '\n';
    }

    void emitExit(const DispatchTransfer& transfer, std::size_t offset)
    {
        output_ << "  switch i64 " << operandName(transfer.selector) << ", label %invalid_cleanup_transfer [\n";
        for (std::size_t index = 0; index < transfer.destinations.size(); ++index)
            output_ << "    i64 " << index << ", label %block" << transfer.destinations[index] << '\n';
        output_ << "  ]";
        location(offset);
        output_ << '\n';
    }

    void saveDebugSlots(const std::string& name)
    {
        if (debug_) {
            for (std::size_t slot = 0; slot < function_.slots.size(); ++slot) {
                const auto type = storageType(function_.slots[slot].type);
                const auto saved = name + ".saved" + std::to_string(slot);
                output_ << "  " << saved << " = load " << type << ", ptr %slot" << slot << '\n'
                        << "  store " << type << ' ' << saved << ", ptr %savedSlot" << slot << '\n';
            }
        }
    }

    void storeStepField(const std::string& name, unsigned field, std::string_view type, std::string_view value)
    {
        output_ << "  " << name << " = getelementptr %pipe.step, ptr %step, i32 0, i32 " << field << '\n'
                << "  store " << type << ' ' << value << ", ptr " << name << '\n';
    }

    void finishSuspension(const std::string& name, std::size_t resume, std::size_t offset)
    {
        emitSourceNote("await", offset);
        storeStepField(name + ".resume", 0, "i64", std::to_string(resume));
        output_ << "  ret void";
        location(offset);
        output_ << '\n';
    }

    void emitExit(const Suspend& suspend, std::size_t offset)
    {
        const auto name = "%suspend" + std::to_string(suspend.resume);
        saveDebugSlots(name);
        storeStepField(name + ".site", 1, "i64", operandName(suspend.semaphore));
        storeStepField(name + ".timeout", 2, "i64", operandName(suspend.timeout));
        if (suspend.cancellation)
            storeStepField(name + ".token", 7, "ptr", operandName(*suspend.cancellation));
        finishSuspension(name, suspend.resume, offset);
    }

    void emitExit(const AwaitCall& call, std::size_t offset)
    {
        const auto name = "%await" + std::to_string(runtimeCalls_++);
        saveDebugSlots(name);
        storeStepField(name + ".callee", 3, "ptr", asyncDescriptor(call.destination));
        if (call.receiver)
            storeStepField(name + ".receiver", 4, "ptr", operandName(*call.receiver));

        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            const auto address = name + ".argument" + std::to_string(index);
            const auto& argument = call.arguments[index];
            const auto slot = call.argumentSlots[index];
            const auto member = argument.type == ScalarType::CancellationToken || slot ? 1 : 0;
            output_ << "  " << address << " = getelementptr %pipe.step, ptr %step, i32 0, i32 5, i64 "
                    << index << ", i32 " << member << '\n';
            if (slot) {
                output_ << "  store ptr " << (debug_ ? "%savedSlot" : "%slot") << *slot << ", ptr " << address << '\n';
            } else {
                auto value = operandName(argument);
                if (argument.type == ScalarType::Boolean) {
                    output_ << "  " << address << ".byte = zext i1 " << value << " to i8\n";
                    value = address + ".byte";
                }
                output_ << "  store " << storageType(argument.type) << ' ' << value << ", ptr " << address << '\n';
            }
        }
        finishSuspension(name, call.resume, offset);
    }

    void emitExit(const AwaitHost& call, std::size_t offset)
    {
        const auto name = "%hostAwait" + std::to_string(call.resume);
        saveDebugSlots(name);
        finishSuspension(name, call.resume, offset);
    }

    void emitMetadata()
    {
        const auto functionLine = line(function_.offset);
        // CodeView makes relative file records absolute using the compiler's
        // working directory. Use a stable Windows path for debugger remapping.
        const auto sourceDirectory = target_.format == ObjectFormat::Coff ? "C:/pipelang-source" : ".";
        // GDB uses the entry source line to recognize these calls. For
        // contextual functions, a frame pointer lets it skip the prologue
        // before reading authored arguments from their initialized slots.
        const auto authoredEntry = function_.mode == FunctionMode::Construction ||
            function_.mode == FunctionMode::Contextual;
        const auto scopeLine = debug_ && authoredEntry ? functionLine : 0;
        output_ << "\n!" << metadataStart_ << " = distinct !DICompileUnit(language: DW_LANG_C, file: !" << file()
                << ", producer: \"PipeLang\", "
                << "isOptimized: " << (debug_ ? "false" : "true")
                << ", runtimeVersion: 0, emissionKind: " << (debug_ ? "FullDebug" : "LineTablesOnly") << ")\n"
                << '!' << file() << " = !DIFile(filename: \"" << llvmString(function_.sourceFile)
                << "\", directory: \"" << sourceDirectory << "\")\n"
                << '!' << subprogram() << " = distinct !DISubprogram(name: \"" << llvmString(function_.name)
                << "\", linkageName: \"" << llvmString(function_.symbol)
                << "\", scope: !" << file() << ", file: !" << file() << ", line: " << functionLine
                << ", scopeLine: " << scopeLine << ", type: !" << signature()
                << ", spFlags: DISPFlagDefinition";
        if (!debug_)
            output_ << " | DISPFlagOptimized";
        output_ << ", unit: !" << metadataStart_ << ")\n"
                << '!' << signature() << " = !DISubroutineType(types: !" << parameterTypes() << ")\n"
                << '!' << parameterTypes() << " = !{"
                << (function_.returnsVoid ? "null" : "!" + std::to_string(debugType(function_.result, layoutMetadata_)));
        for (std::size_t index = 0; index < function_.parameterCount; ++index)
            output_ << ", !" << debugType(function_.slots[index].type, layoutMetadata_);
        output_ << "}\n";

        if (debug_)
            emitLocalMetadata();

        for (const auto& [sourceLine, id] : locations_)
            output_ << '!' << id << " = !DILocation(line: " << sourceLine << ", scope: !" << subprogram() << ")\n";
    }

    void emitLocalMetadata()
    {
        for (std::size_t index = 0; index < function_.slots.size(); ++index) {
            const auto& slot = function_.slots[index];
            if (slot.name.empty())
                continue;
            output_ << '!' << slotMetadata(index) << " = !DILocalVariable(name: \"" << llvmString(slot.name) << '"';
            if (index < function_.parameterCount)
                output_ << ", arg: " << index + 1;
            output_ << ", scope: !" << subprogram() << ", file: !" << file() << ", line: " << line(slot.offset)
                    << ", type: !" << debugType(slot.type, layoutMetadata_) << ")\n";
        }
    }

    const ScalarFunction& function_;
    const Target& target_;
    bool debug_;
    std::size_t metadataStart_;
    std::span<const ValueLayout> layouts_;
    std::span<const std::size_t> layoutMetadata_;
    bool tracksSourceCallDepth_;
    bool retainsObjectGraphs_;
    bool allowsMultipleTasks_;
    bool activeHostFloating_;
    bool retainsScopedBytes_;
    bool hasFaultTargets_ = false;
    bool hasHostedResultGuard_ = false;
    bool usesFloating_ = false;
    std::size_t currentFaultTarget_ = 0;
    struct SourceGuard {
        std::string_view diagnostic;
        std::size_t offset;
        std::size_t handler;
    };
    std::vector<SourceGuard> sourceGuards_;
    std::string traceFunction_;
    std::string traceFile_;
    std::vector<std::size_t> newlines_;
    std::map<std::size_t, std::size_t> locations_;
    std::size_t booleanStores_ = 0;
    std::size_t runtimeCalls_ = 0;
    std::size_t runtimeBooleans_ = 0;
    std::size_t fields_ = 0;
    std::ostringstream textGlobals_;
    std::ostringstream output_;
};

std::string_view numericAbiType(NativeType type)
{
    switch (type.kind) {
    case ScalarType::Record: return "ptr";
    case ScalarType::Float: return "double";
    case ScalarType::Float32: return "float";
    case ScalarType::UInt32: return "i32";
    case ScalarType::Integer: return "i64";
    default: throw std::logic_error("unsupported numeric ABI type");
    }
}

std::string numericCValueType(NativeType type)
{
    if (type == ScalarType::Record)
        return "%pipe.numeric." + std::to_string(type.layout);
    return std::string(numericAbiType(type));
}

enum class NumericRecordDirection { Import, Export };

class NumericRecordBridge {
public:
    NumericRecordBridge(std::ostream& output, std::span<const ValueLayout> layouts)
        : output_(output), layouts_(layouts) {}

    std::string convert(NativeType type, std::string_view input, NumericRecordDirection direction)
    {
        const bool importing = direction == NumericRecordDirection::Import;
        if (type == ScalarType::Integer)
            return std::string(input);

        if (type == ScalarType::Record) {
            const auto inputType = importing ? numericCValueType(type) : llvmType(type);
            const auto outputType = importing ? llvmType(type) : numericCValueType(type);
            const auto& fields = layouts_[type.layout].fields;
            std::string result = "poison";
            for (std::size_t index = 0; index < fields.size(); ++index) {
                const auto field = next();
                output_ << "  " << field << " = extractvalue " << inputType << ' ' << input << ", " << index << '\n';
                const auto converted = convert(fields[index].type, field, direction);
                const auto convertedType = importing ? llvmType(fields[index].type) : numericCValueType(fields[index].type);
                const auto updated = next();
                output_ << "  " << updated << " = insertvalue " << outputType << ' ' << result << ", "
                        << convertedType << ' ' << converted << ", " << index << '\n';
                result = updated;
            }
            return result;
        }

        const auto first = next();
        if (type == ScalarType::Float) {
            const auto external = numericCValueType(type);
            output_ << "  " << first << " = bitcast " << (importing ? external : "i64") << ' ' << input
                    << " to " << (importing ? "i64" : external) << '\n';
            return first;
        }

        if (type == ScalarType::UInt32) {
            output_ << "  " << first << (importing ? " = zext i32 " : " = trunc i64 ") << input
                    << (importing ? " to i64\n" : " to i32\n");
            return first;
        }

        if (type != ScalarType::Float32)
            throw std::logic_error("unsupported numeric ABI record field");
        const auto second = next();
        if (importing) {
            output_ << "  " << first << " = bitcast float " << input << " to i32\n"
                    << "  " << second << " = zext i32 " << first << " to i64\n";
        } else {
            output_ << "  " << first << " = trunc i64 " << input << " to i32\n"
                    << "  " << second << " = bitcast i32 " << first << " to float\n";
        }
        return second;
    }

private:
    std::string next()
    {
        return "%bridge" + std::to_string(nextRegister_++);
    }

    std::ostream& output_;
    std::span<const ValueLayout> layouts_;
    std::size_t nextRegister_ = 0;
};

void emitNumericRecordTypes(std::ostream& output, std::span<const ValueLayout> layouts,
                            std::set<std::size_t>& emitted, NativeType type)
{
    if (type != ScalarType::Record || !emitted.insert(type.layout).second)
        return;

    output << numericCValueType(type) << " = type {";
    const auto& fields = layouts[type.layout].fields;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << numericCValueType(fields[index].type);
    }
    output << "}\n";
    for (const auto& field : fields)
        emitNumericRecordTypes(output, layouts, emitted, field.type);
}

void emitNumericExport(std::ostream& output, const ScalarFunction& entry, std::string_view symbol,
                       std::span<const ValueLayout> layouts)
{
    const auto parameters = std::span(entry.slots).first(entry.parameterCount);
    std::set<std::size_t> recordTypes;
    emitNumericRecordTypes(output, layouts, recordTypes, entry.result);
    for (const auto& parameter : parameters)
        emitNumericRecordTypes(output, layouts, recordTypes, parameter.type);
    const bool returnsRecord = entry.result == ScalarType::Record;
    output << "\ndefine " << (returnsRecord ? "void" : numericAbiType(entry.result))
           << " @\"" << llvmString(symbol) << "\"(";
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << numericAbiType(parameters[index].type) << " %argument" << index;
    }
    if (returnsRecord) {
        if (!parameters.empty())
            output << ", ";
        output << "ptr %output";
    }
    output << ") strictfp {\n";
    // Marshal the C struct and the compiler's internal word layout field by field.
    NumericRecordBridge bridge(output, layouts);
    std::vector<std::string> recordArguments(parameters.size());
    // Keep floating inputs as word storage inside the lowered body without
    // changing how C callers pass them or observe the active environment.
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto type = parameters[index].type;
        if (type == ScalarType::Record) {
            const auto input = "%record" + std::to_string(index);
            output << "  " << input << " = load " << numericCValueType(type) << ", ptr %argument" << index << "\n";
            recordArguments[index] = bridge.convert(type, input, NumericRecordDirection::Import);
        } else if (type == ScalarType::Float) {
            output << "  %word" << index << " = bitcast double %argument" << index << " to i64\n";
        } else if (type == ScalarType::Float32) {
            output << "  %bits" << index << " = bitcast float %argument" << index << " to i32\n"
                   << "  %word" << index << " = zext i32 %bits" << index << " to i64\n";
        } else if (type == ScalarType::UInt32) {
            output << "  %word" << index << " = zext i32 %argument" << index << " to i64\n";
        }
    }
    output << "  %result = call " << llvmType(entry.result) << " @\"" << llvmString(entry.symbol) << "\"(";
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0)
            output << ", ";
        const auto type = parameters[index].type;
        output << llvmType(type) << ' ';
        if (type == ScalarType::Record)
            output << recordArguments[index];
        else
            output << (type == ScalarType::Integer ? "%argument" : "%word") << index;
    }
    output << ")\n";
    if (returnsRecord) {
        const auto value = bridge.convert(entry.result, "%result", NumericRecordDirection::Export);
        output << "  store " << numericCValueType(entry.result) << ' ' << value << ", ptr %output\n  ret void\n";
    } else if (entry.result == ScalarType::Float) {
        output << "  %value = bitcast i64 %result to double\n  ret double %value\n";
    } else if (entry.result == ScalarType::Float32) {
        output << "  %bits = trunc i64 %result to i32\n"
                  "  %value = bitcast i32 %bits to float\n  ret float %value\n";
    } else if (entry.result == ScalarType::UInt32) {
        output << "  %value = trunc i64 %result to i32\n  ret i32 %value\n";
    } else {
        output << "  ret i64 %result\n";
    }
    output << "}\n";
}

std::string emitHostedConstructor(std::ostream& output, const std::string& prefix, std::string_view constructor,
                                  std::span<const HostedParameter> parameters, std::size_t receiverTag, bool factory = false,
                                  std::optional<std::size_t> cancellation = std::nullopt)
{
    const auto count = parameters.size();
    std::string bindings = "null";
    if (count != 0) {
        for (std::size_t index = 0; index < count; ++index) {
            const auto& service = parameters[index].service;
            if (!service.empty()) {
                output << prefix << ".service." << index << " = private unnamed_addr constant ["
                       << service.size() + 1 << " x i8] c\"" << llvmString(service) << "\\00\"\n";
            }
        }
        bindings = prefix + ".parameters";
        output << bindings << " = private constant [" << count << " x {ptr, ptr}] [";
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0)
                output << ", ";
            const auto& parameter = parameters[index];
            if (parameter.service.empty())
                output << "{ptr, ptr} {ptr " << ownedShapeName(parameter.type) << ", ptr null}";
            else
                output << "{ptr, ptr} {ptr null, ptr " << prefix << ".service." << index << '}';
        }
        output << "]\n";
    }

    output << "define internal i32 " << prefix << ".construct(ptr %receiver, ptr %arguments, "
              "ptr %arena, ptr %allocateReceiver";
    if (factory)
        output << ", ptr %cancellation";
    output << ") {\n";
    if (!factory && receiverTag != 0) {
        output << "  %receiver.tag = getelementptr i64, ptr %receiver, i64 "
               << native_app_v19::receiverTagWord << '\n'
               << "  store i64 " << receiverTag << ", ptr %receiver.tag\n";
    }
    for (std::size_t index = 0; index < count; ++index) {
        output << "  %arg" << index << ".slot = getelementptr ptr, ptr %arguments, i64 " << index << '\n'
               << "  %arg" << index << ".address = load ptr, ptr %arg" << index << ".slot\n"
               << "  %arg" << index << " = load " << llvmType(parameters[index].type)
               << ", ptr %arg" << index << ".address\n";
    }
    output << "  %status = call i32 @\"" << llvmString(constructor)
           << "\"(ptr %arena, ptr %allocateReceiver, ptr %receiver";
    for (std::size_t index = 0; index < count; ++index) {
        if (cancellation == index)
            output << ", ptr %cancellation";
        output << ", " << llvmType(parameters[index].type) << " %arg" << index;
    }
    if (cancellation == count)
        output << ", ptr %cancellation";
    output << ")\n  ret i32 %status\n}\n";
    return bindings;
}

void emitHostedCompositionPlan(std::ostream& output, std::span<const HostedCompositionService> nodes)
{
    if (nodes.empty())
        return;

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const auto prefix = "@.pipe.host.plan." + std::to_string(index);
        output << prefix << ".name = private unnamed_addr constant [" << node.name.size() + 1
               << " x i8] c\"" << llvmString(node.name) << "\\00\"\n";
        if (node.dependencies.empty())
            continue;

        output << prefix << ".dependencies = private constant [" << node.dependencies.size() << " x i64] [";
        for (std::size_t dependency = 0; dependency < node.dependencies.size(); ++dependency) {
            if (dependency != 0)
                output << ", ";
            output << "i64 " << node.dependencies[dependency];
        }
        output << "]\n";
    }

    output << "%pipe.host.plan = type {ptr, i64, i64, i64, i64, ptr, i64, ptr}\n"
           << "@.pipe.host.plan = private constant [" << nodes.size() << " x %pipe.host.plan] [";
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (index != 0)
            output << ", ";
        const auto& node = nodes[index];
        const auto prefix = "@.pipe.host.plan." + std::to_string(index);
        output << "%pipe.host.plan {ptr " << prefix << ".name, i64 " << node.lifetime << ", i64 " << node.input
               << ", i64 " << node.flags << ", i64 " << node.recipe << ", ptr "
               << (node.dependencies.empty() ? "null" : prefix + ".dependencies")
               << ", i64 " << node.dependencies.size() << ", ptr "
               << (node.snapshot ? ownedShapeName(*node.snapshot) : "null") << '}';
    }
    output << "]\n";
}

void emitHostedCleanup(std::ostream& output, const std::string& prefix, std::string_view cleanup)
{
    output << "define internal i32 " << prefix << ".cleanup(ptr %result, ptr %receiver, ptr %arena, "
           << "ptr %allocateReceiver, ptr %cancellation) {\n"
           << "  %status = call i32 @\"" << llvmString(cleanup)
           << "\"(ptr %arena, ptr %allocateReceiver, ptr %result, ptr %receiver, ptr %cancellation)\n"
           << "  ret i32 %status\n}\n";
}

struct HostedRecipeTables {
    bool factories = false;
    bool cleanups = false;
};

HostedRecipeTables emitHostedComposition(std::ostream& output, const HostedEntry& descriptor)
{
    if (descriptor.recipes.empty())
        return {};

    std::vector<std::string> bindings;
    bindings.reserve(descriptor.recipes.size());
    for (std::size_t index = 0; index < descriptor.recipes.size(); ++index) {
        const auto& recipe = descriptor.recipes[index];
        const auto prefix = "@.pipe.host.recipe." + std::to_string(index);
        output << prefix << ".name = private unnamed_addr constant [" << recipe.service.size() + 1
               << " x i8] c\"" << llvmString(recipe.service) << "\\00\"\n";
        bindings.push_back(emitHostedConstructor(output, prefix, recipe.callable, recipe.parameters, recipe.receiverTag,
                                                  recipe.factory, recipe.cancellationIndex));
        if (!recipe.cleanup.empty())
            emitHostedCleanup(output, prefix, recipe.cleanup);
    }

    output << "%pipe.host.recipe = type {ptr, ptr, ptr, i64, i64, ptr, i64}\n"
           << "@.pipe.host.recipes = private constant [" << descriptor.recipes.size() << " x %pipe.host.recipe] [";
    for (std::size_t index = 0; index < descriptor.recipes.size(); ++index) {
        if (index != 0)
            output << ", ";
        const auto& recipe = descriptor.recipes[index];
        const auto prefix = "@.pipe.host.recipe." + std::to_string(index);
        output << "%pipe.host.recipe {ptr " << prefix << ".name, ptr " << ownedShapeName(recipe.receiverType)
               << ", ptr " << (recipe.factory ? "null" : prefix + ".construct")
               << ", i64 " << recipe.receiverWords << ", i64 " << recipe.receiverTag
               << ", ptr " << bindings[index] << ", i64 " << recipe.parameters.size() << '}';
    }
    output << "]\n";

    const bool hasFactories = std::ranges::any_of(descriptor.recipes, &HostedRecipeEntry::factory);
    if (hasFactories) {
        output << "@.pipe.host.factories = private constant [" << descriptor.recipes.size() << " x ptr] [";
        for (std::size_t index = 0; index < descriptor.recipes.size(); ++index) {
            if (index != 0)
                output << ", ";
            if (descriptor.recipes[index].factory)
                output << "ptr @.pipe.host.recipe." << index << ".construct";
            else
                output << "ptr null";
        }
        output << "]\n";
    }
    const bool hasCleanups = std::ranges::any_of(descriptor.recipes,
        [](const HostedRecipeEntry& recipe) { return !recipe.cleanup.empty(); });
    if (hasCleanups) {
        output << "@.pipe.host.cleanups = private constant [" << descriptor.recipes.size() << " x ptr] [";
        for (std::size_t index = 0; index < descriptor.recipes.size(); ++index) {
            if (index != 0)
                output << ", ";
            if (descriptor.recipes[index].cleanup.empty())
                output << "ptr null";
            else
                output << "ptr @.pipe.host.recipe." << index << ".cleanup";
        }
        output << "]\n";
    }
    return {hasFactories, hasCleanups};
}

}

std::string emitLlvmIr(const ScalarProgram& program, const std::vector<ModuleSource>& sources,
                       const Target& target, bool debug, bool sourceTrace, bool activeHostFloating)
{
    const auto& functions = program.functions;
    const auto triple = functions.back().usesRuntime() && !target.runtimeTriple.empty()
        ? target.runtimeTriple : target.triple;
    std::ostringstream output;
    output << "source_filename = \"" << llvmString(functions.back().sourceFile) << "\"\n"
           << "target triple = \"" << triple << "\"\n\n";
    if (debug)
        output << "declare void @llvm.dbg.declare(metadata, metadata, metadata)\n"
                  "declare void @llvm.dbg.value(metadata, metadata, metadata)\n"
                  "declare void @llvm.dbg.addr(metadata, metadata, metadata)\n\n";
    if (sourceTrace)
        output << "declare void @pipelang_native_v19_note_failure(ptr, ptr, ptr, i64)\n"
                  "declare void @pipelang_native_v19_note_await(ptr, ptr, ptr, i64)\n";
    if (activeHostFloating)
        output << "declare float @llvm.experimental.constrained.fptrunc.f32.f64(double, metadata, metadata)\n";
    if (program.numericExport) {
        output << "declare double @llvm.experimental.constrained.fpext.f64.f32(float, metadata)\n"
                  "declare float @llvm.experimental.constrained.sitofp.f32.i64(i64, metadata, metadata)\n"
                  "declare double @llvm.experimental.constrained.sitofp.f64.i64(i64, metadata, metadata)\n";
    }
    for (const auto type : {NativeType{ScalarType::Float}, NativeType{ScalarType::Float32}}) {
        const auto* floating = llvmFloatingType(type);
        for (const auto* opcode : {"fadd", "fsub", "fmul", "fdiv"})
            output << "declare " << floating << " @llvm.experimental.constrained." << opcode << '.'
                   << floatingSuffix(type) << '(' << floating << ", " << floating << ", metadata, metadata)\n";
        for (const auto* opcode : {"fcmp", "fcmps"})
            output << "declare i1 @llvm.experimental.constrained." << opcode << '.'
                   << floatingSuffix(type) << '(' << floating << ", " << floating << ", metadata, metadata)\n";
    }
    if (functions.back().usesRuntime() || program.numericExport) {
        output << "%pipe.text = type {ptr, i64}\n%pipe.bytes = type {i64, i64, i64}\n";
        for (std::size_t index = 0; index < program.layouts.size(); ++index) {
            const auto& layout = program.layouts[index];
            if (layout.kind == ScalarType::Enum)
                continue;
            const auto& fields = layout.fields;
            if (layout.kind == ScalarType::FixedArray) {
                output << "%pipe.value." << index << " = type ";
                if (layout.fixedExtent == 0)
                    output << "{i64}\n";
                else
                    output << '[' << layout.fixedExtent << " x " << fieldStorageType(fields.front().type) << "]\n";
                continue;
            }
            output << "%pipe.value." << index << " = type {";
            if (usesCollectionHandle(layout.kind)) {
                output << (isSequence(layout.kind) ? "i64, i64, i64}\n" : "i64, i64}\n");
                continue;
            }
            if (fields.empty())
                output << "i64";
            for (std::size_t field = 0; field < fields.size(); ++field) {
                if (field != 0)
                    output << ", ";
                output << fieldStorageType(fields[field].type);
            }
            output << "}\n";
        }
    }
    if (functions.back().usesRuntime()) {
        {
            using native_app_v19::OwnedKind;
            output << "%pipe.owned = type {i64, i64, i64, ptr, ptr, i64, ptr}\n"
                   << "%pipe.owned.field = type {i64, ptr, ptr}\n";
            std::map<std::string, std::string> names;
            const auto intern = [&](const std::string& value) {
                const auto [entry, inserted] = names.try_emplace(value, "@.pipe.owned.name." + std::to_string(names.size()));
                if (inserted) {
                    output << entry->second << " = private unnamed_addr constant [" << value.size() + 1
                           << " x i8] c\"" << llvmString(value) << "\\00\"\n";
                }
                return entry->second;
            };
            const auto shape = [&](const std::string& name, OwnedKind kind, std::size_t words,
                                   std::size_t fields, const std::string& children, const std::string& typeName,
                                   std::size_t valueCount = 0, const std::string& enumNames = "null") {
                const auto type = intern(typeName);
                output << name << " = private constant %pipe.owned {i64 " << static_cast<std::uint64_t>(kind)
                       << ", i64 " << words << ", i64 " << fields << ", ptr " << children << ", ptr " << type
                       << ", i64 " << valueCount << ", ptr " << enumNames << "}\n";
            };
            shape(ownedShapeName(ScalarType::Integer), OwnedKind::Integer, 1, 0, "null", "int");
            shape(ownedShapeName(ScalarType::UInt32), OwnedKind::UInt32, 1, 0, "null", "uint32");
            shape(ownedShapeName(ScalarType::Float), OwnedKind::Float, 1, 0, "null", "float");
            shape(ownedShapeName(ScalarType::Float32), OwnedKind::Float32, 1, 0, "null", "float32");
            shape(ownedShapeName(ScalarType::ByteBuffer), OwnedKind::ByteBuffer, 3, 0, "null", "ByteBuffer");
            shape(ownedShapeName(ScalarType::Boolean), OwnedKind::Boolean, 1, 0, "null", "bool");
            shape(ownedShapeName(ScalarType::Text), OwnedKind::Text, 2, 0, "null", "string");
            shape(ownedShapeName(ScalarType::Error), OwnedKind::Error, 2, 0, "null", "Error");
            shape(ownedShapeName(ScalarType::Interface), OwnedKind::ObjectReference, 1, 0, "null", "source interface");
            shape(ownedShapeName(ScalarType::Semaphore), OwnedKind::Reference, 1, 0, "null", "SemaphoreSlim");
            shape(ownedShapeName(ScalarType::LockReference), OwnedKind::Reference, 1, 0, "null", "Lock");
            shape(ownedShapeName(ScalarType::ContextOwnerReference), OwnedKind::ContextOwnerReference, 1, 0, "null",
                  "ContextOwner");

            for (std::size_t index = 0; index < program.layouts.size(); ++index) {
                const auto& layout = program.layouts[index];
                const bool referenceMetadata = layout.kind == ScalarType::AtomicReference ||
                    layout.kind == ScalarType::ConcurrentSequenceReference ||
                    layout.kind == ScalarType::ConcurrentDictionaryReference;
                const auto fields = referenceMetadata ? 0 : layout.fields.size();
                auto children = std::string("null");
                if (fields != 0) {
                    std::vector<std::string> fieldNames;
                    for (const auto& field : layout.fields)
                        fieldNames.push_back(intern(field.name));
                    children = "@.pipe.owned.fields." + std::to_string(index);
                    output << children << " = private constant [" << fields << " x %pipe.owned.field] [";
                    std::size_t offset = 0;
                    for (std::size_t field = 0; field < fields; ++field) {
                        if (field != 0)
                            output << ", ";
                        const auto type = layout.fields[field].type;
                        output << "%pipe.owned.field {i64 " << offset << ", ptr " << ownedShapeName(type)
                               << ", ptr " << fieldNames[field] << '}';
                        offset += valueWords(type, program.layouts);
                    }
                    output << "]\n";
                }
                auto kind = OwnedKind::Record;
                if (layout.kind == ScalarType::Result)
                    kind = OwnedKind::Result;
                else if (layout.kind == ScalarType::ResultInterface)
                    kind = OwnedKind::ResultInterface;
                else if (layout.kind == ScalarType::List)
                    kind = OwnedKind::List;
                else if (layout.kind == ScalarType::HashSet)
                    kind = OwnedKind::HashSet;
                else if (layout.kind == ScalarType::Queue)
                    kind = OwnedKind::Queue;
                else if (layout.kind == ScalarType::Stack)
                    kind = OwnedKind::Stack;
                else if (layout.kind == ScalarType::Dictionary)
                    kind = OwnedKind::Dictionary;
                else if (layout.kind == ScalarType::Pair)
                    kind = OwnedKind::Pair;
                else if (layout.kind == ScalarType::Enum)
                    kind = OwnedKind::Enum;
                else if (layout.kind == ScalarType::Optional)
                    kind = OwnedKind::Optional;
                else if (layout.kind == ScalarType::ClassReference)
                    kind = OwnedKind::ObjectReference;
                else if (referenceMetadata)
                    kind = OwnedKind::Reference;
                else if (layout.kind == ScalarType::FixedArray)
                    kind = OwnedKind::FixedArray;
                else if (layout.kind == ScalarType::Array)
                    kind = OwnedKind::Array;
                auto enumSymbols = std::string("null");
                if (!layout.enumerators.empty()) {
                    std::vector<std::string> members;
                    for (const auto& member : layout.enumerators)
                        members.push_back(intern(member));
                    enumSymbols = "@.pipe.owned.enumerators." + std::to_string(index);
                    output << enumSymbols << " = private constant [" << members.size() << " x ptr] [";
                    for (std::size_t member = 0; member < members.size(); ++member) {
                        if (member != 0)
                            output << ", ";
                        output << "ptr " << members[member];
                    }
                    output << "]\n";
                }
                const auto valueCount = layout.kind == ScalarType::FixedArray ? layout.fixedExtent
                                                                             : layout.enumerators.size();
                const NativeType type{layout.kind, index};
                shape(ownedShapeName(type), kind, valueWords(type, program.layouts), fields, children,
                      layout.name, valueCount, enumSymbols);
            }
        }
        output << "%pipe.argument = type {i64, ptr}\n"
               << "%pipe.host.argument = type {ptr, ptr}\n"
               << "%pipe.step = type {i64, i64, i64, ptr, ptr, [" << native_app_v19::maximumArguments
               << " x %pipe.argument], i64, ptr}\n"
               << "%pipe.async = type {ptr, i64, i64, i64, i64, i64}\n"
               << "declare i32 @pipelang_native_v19_array(ptr, i32, ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_initialization_array(ptr, i32, ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_list_append(ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_list_read(ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_sequence(ptr, i32, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_sequence(ptr, i32, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_dictionary(ptr, i32, ptr, ptr, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_dictionary(ptr, i32, ptr, ptr, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_set_operation(ptr, i32, ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_set_operation(ptr, i32, ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_text_operation(ptr, i32, ptr, ptr, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_initialization_text(ptr, i32, ptr, ptr, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_owned_equal(ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_initialization_equal(ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_erase_result(ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_erase_hosted_result(ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_validate_constructed_value(ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_validate_hosted_value(ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_require_result_interface(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_hosted_require_result_interface(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_host_call(ptr, i64, ptr, ptr, ptr, i64, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_host_call_with_reusable_bytes(ptr, i64, ptr, ptr, ptr, i64, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_initialization_call(ptr, i64, ptr, ptr, ptr, i64, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_prepare_host_call(ptr, i64, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_step(ptr)\n"
               << "declare i32 @pipelang_native_v19_enter_source_call(ptr)\n"
               << "declare void @pipelang_native_v19_leave_source_call(ptr)\n"
               << "declare i32 @pipelang_native_v19_charge(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_cancellation_requested(ptr)\n"
               << "declare i32 @pipelang_native_v19_begin_cleanup(ptr)\n"
               << "declare i32 @pipelang_native_v19_end_cleanup(ptr)\n"
               << "declare void @pipelang_native_v19_route_fault(ptr)\n"
               << "declare i32 @pipelang_native_v19_capture_error(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_import_process_arguments(ptr, ptr, ptr, ptr)\n"
               << "declare void @pipelang_native_v19_hosted_route_fault(ptr, i32)\n"
               << "declare i32 @pipelang_native_v19_hosted_begin_cleanup(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_begin_construction(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_begin_tracked_construction(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_enter_source_call(ptr)\n"
               << "declare void @pipelang_native_v19_hosted_leave_source_call(ptr)\n"
               << "declare void @pipelang_native_v19_hosted_finish_construction(ptr, i32)\n"
               << "declare i32 @pipelang_native_v19_hosted_track_receiver(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_list_append(ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_list_read(ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_end_cleanup(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_capture_error(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_raise_error(ptr, ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_hosted_raise_error(ptr, ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_hosted_invalid_result_value(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_invalid_optional_value(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_atomic_call(ptr, i64, i32, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_atomic(ptr, ptr, i64, i32, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_invalid_dispatch(ptr)\n"
               << "declare i32 @pipelang_native_v19_integer(ptr, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_integer(ptr, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_charge_hosted_initialization(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_uint32(ptr, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_uint32(ptr, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_floating(ptr, i32, i32, i64, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_floating(ptr, i32, i32, i64, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_bytes(ptr, i32, ptr, ptr, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_initialization_bytes(ptr, i32, ptr, ptr, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_retain_receiver_bytes(ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_record_byte_fields(ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_bytes(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_record(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_collections(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_write_receiver_collections(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_sets(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_values(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_synchronization(ptr, ptr, i64, i32, i64)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_reference(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_values(ptr, ptr, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_sets(ptr, ptr, i64, ptr, ptr)\n"
               << "declare void @pipelang_native_v19_invalid_result_value(ptr)\n"
               << "declare void @pipelang_native_v19_invalid_optional_value(ptr)\n"
               << "declare void @pipelang_native_v19_invalid_fixed_array_index(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_invalid_fixed_array_index(ptr)\n"
               << "declare i32 @pipelang_native_v19_validate_fixed_array(ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_validate_fixed_array(ptr, ptr, ptr)\n"
               << "declare void @pipelang_native_v19_uninitialized_receiver(ptr)\n"
               << "declare void @pipelang_native_v19_uninitialized_context_owner(ptr)\n"
               << "declare ptr @pipelang_native_v19_allocate_receiver(ptr, i64, i64)\n"
               << "declare i32 @pipelang_native_v19_begin_construction(ptr, i32)\n"
               << "declare void @pipelang_native_v19_finish_construction(ptr, i32)\n"
               << "declare ptr @pipelang_native_v19_allocate_constructed_receiver(ptr, i64, i64)\n"
               << "declare ptr @pipelang_native_v19_allocate_constructed_graph_receiver(ptr, i64, i64)\n"
               << "declare i32 @pipelang_native_v19_track_receiver(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_borrow_receiver_bytes(ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_write_receiver_text(ptr, ptr, i64, ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_construction_status(ptr, i32)\n"
               << "declare void @pipelang_native_v19_invalid_dispatch(ptr)\n"
               << "declare i32 @pipelang_native_v19_semaphore(ptr, i64, i64, i64, i32)\n"
               << "declare i32 @pipelang_native_v19_create_context_owner(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_context_owner(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_context_owner_call(ptr, i64, i32)\n"
               << "declare i32 @pipelang_native_v19_hosted_context_owner_call(ptr, i64, i32)\n"
               << "declare i32 @pipelang_native_v19_create_semaphore(ptr, i64, i64, i32, ptr)\n"
               << "declare i32 @pipelang_native_v19_share_semaphore(ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_semaphore_count(ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_semaphore_count(ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_semaphore(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_semaphore(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_pin_receiver(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_concurrent_dictionary(ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_concurrent_dictionary(ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_concurrent_dictionary(ptr, ptr, i64, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_concurrent_dictionary(ptr, ptr, i64, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_call_concurrent_dictionary(ptr, i64, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_call_concurrent_dictionary(ptr, i64, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_concurrent_sequence(ptr, i32, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_concurrent_sequence(ptr, i32, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_concurrent_sequence(ptr, ptr, i64, i32, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_concurrent_sequence(ptr, ptr, i64, i32, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_call_concurrent_sequence(ptr, i64, i32, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_call_concurrent_sequence(ptr, i64, i32, ptr, ptr, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_parallel_owned(ptr, ptr, ptr, ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_parallel_owned_cancellable(ptr, ptr, ptr, ptr, ptr, i64, ptr, ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_hosted_parallel_owned(ptr, ptr, ptr, ptr, ptr, i64, ptr, ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_wait_semaphore(ptr, i64, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_wait_semaphore(ptr, i64, i64, ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_lock(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_lock(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_read_receiver_lock(ptr, ptr, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_acquire_lock(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_hosted_acquire_lock(ptr, i64)\n"
               << "declare i32 @pipelang_native_v19_release_lock(ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_release_lock(ptr)\n"
               << "declare i32 @pipelang_native_v19_create_atomic(ptr, i32, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_record_constructed_receiver_field(ptr, ptr, ptr, ptr, i32)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_atomic(ptr, i32, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_lock(ptr, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_hosted_semaphore(ptr, i64, i64, i32, ptr)\n"
               << "declare i32 @pipelang_native_v19_create_local_atomic(ptr, i64, i32, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_atomic_call(ptr, i64, i32, i32, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_read_receiver_atomic(ptr, ptr, i64, i32, ptr)\n"
               << "declare i32 @pipelang_native_v19_release_semaphore(ptr, i64, i64, ptr)\n"
               << "declare i32 @pipelang_native_v19_hosted_release_semaphore(ptr, i64, i64, ptr)\n";
        for (const auto& function : functions) {
            if (!function.continuation())
                continue;
            const auto frameWords = slotStorageWords(function, program.layouts) + function.receiverStorageWords;
            const bool hasReceiver = function.parameterCount != 0 && function.slots.front().type == ScalarType::Receiver;
            const std::size_t argumentBegin = hasReceiver ? 1 : 0;
            std::uint64_t cancellationArguments = 0;
            if (function.exported && sourceTrace)
                cancellationArguments |= native_app_v19::sourceTraceCapability;
            if (!program.hosted && takesProcessArguments(function))
                cancellationArguments |= native_app_v19::processArgumentsCapability;
            if (function.checksInvocationCancellation)
                cancellationArguments |= native_app_v19::synchronousCancellationCapability;
            if (function.checksPortableBinary32OnPoll)
                cancellationArguments |= native_app_v19::portableBinary32PollCapability;
            for (std::size_t index = argumentBegin; index < function.parameterCount; ++index) {
                if (function.slots[index].type == ScalarType::CancellationToken)
                    cancellationArguments |= std::uint64_t{1} << (index - argumentBegin);
            }
            if (function.exported && !program.hosted)
                output << "@pipelang_native_v19_entry = constant ";
            else
                output << asyncDescriptor(function.symbol) << " = private constant ";
            output << "%pipe.async {ptr @\""
                   << llvmString(function.symbol) << "\", i64 " << frameWords << ", i64 "
                   << function.parameterCount - argumentBegin << ", i64 " << cancellationArguments << ", i64 "
                   << valueWords(function.result, program.layouts) << ", i64 " << function.childResultWords << "}\n";
        }
        if (program.hosted) {
            const auto& descriptor = *program.hosted;
            const auto& entry = functions.back();
            const auto payloadShape = [&](const ScalarFunction& function) {
                const auto& result = program.layouts[function.result.layout];
                return result.fields.size() == 3 ? ownedShapeName(result.fields[2].type) : std::string("null");
            };
            bool sharedArguments = false;
            bool collectionArguments = false;
            const auto describeArguments = [&](const ScalarFunction& function, std::string table) {
                const auto callArguments = std::span(function.slots).subspan(1, function.parameterCount - 1);
                const bool owned = std::any_of(callArguments.begin(), callArguments.end(), [](const Slot& slot) {
                    return slot.type != ScalarType::Integer && slot.type != ScalarType::CancellationToken;
                });
                if (!owned)
                    return std::pair{std::string("null"), std::uint64_t{0}};

                std::uint64_t indirect = 0;
                output << table << " = private constant [" << callArguments.size() << " x ptr] [";
                for (std::size_t index = 0; index < callArguments.size(); ++index) {
                    if (index)
                        output << ", ";
                    const auto type = callArguments[index].type;
                    const bool sharedSequence = type == ScalarType::ConcurrentSequenceReference;
                    const bool sharedDictionary = type == ScalarType::ConcurrentDictionaryReference;
                    if (sharedDictionary || (sharedSequence && program.layouts[type.layout].name != "ConcurrentQueue<int>"))
                        collectionArguments = true;
                    sharedArguments = sharedArguments || type == ScalarType::Semaphore ||
                        type == ScalarType::LockReference || type == ScalarType::AtomicReference || sharedSequence || sharedDictionary;
                    output << "ptr " << (type == ScalarType::CancellationToken ? "null" : ownedShapeName(type));
                    if (indirectArgument(type))
                        indirect |= std::uint64_t{1} << index;
                }
                output << "]\n";
                return std::pair{std::move(table), indirect};
            };
            const auto entryArguments = describeArguments(entry, "@.pipe.host.arguments");
            const auto emitMethods = [&](std::span<const HostedMethodEntry> methods, const std::string& prefix,
                                         bool firstIsEntry) {
                std::vector<std::pair<std::string, std::uint64_t>> methodArguments;
                methodArguments.reserve(methods.size());
                for (std::size_t index = 0; index < methods.size(); ++index) {
                    const auto& method = methods[index];
                    if (index == 0 && firstIsEntry)
                        methodArguments.push_back(entryArguments);
                    else
                        methodArguments.push_back(describeArguments(functions[method.function],
                            prefix + ".method.arguments." + std::to_string(index)));
                    output << prefix << ".method.name." << index << " = private unnamed_addr constant ["
                           << method.name.size() + 1 << " x i8] c\"" << llvmString(method.name) << "\\00\"\n";
                }
                const auto* type = "{ptr, ptr, ptr, ptr, i64}";
                output << prefix << ".methods = private constant [" << methods.size() << " x " << type
                       << "] [";
                for (std::size_t index = 0; index < methods.size(); ++index) {
                    if (index)
                        output << ", ";
                    const auto& method = functions[methods[index].function];
                    output << type << " {ptr " << prefix << ".method.name." << index << ", ptr "
                           << asyncDescriptor(method.symbol) << ", ptr " << payloadShape(method) << ", ptr "
                           << methodArguments[index].first << ", i64 " << methodArguments[index].second << '}';
                }
                output << "]\n";
            };
            const bool hasMethods = !descriptor.methods.empty();
            if (hasMethods)
                emitMethods(descriptor.methods, "@.pipe.host", true);

            const bool hasServiceMethods = std::ranges::any_of(descriptor.composition,
                [](const HostedCompositionService& service) { return !service.methods.empty(); });
            if (hasServiceMethods) {
                for (std::size_t index = 0; index < descriptor.composition.size(); ++index) {
                    const auto& methods = descriptor.composition[index].methods;
                    if (!methods.empty())
                        emitMethods(methods, "@.pipe.host.plan." + std::to_string(index), false);
                }

                output << "%pipe.host.service.methods = type {ptr, i64}\n"
                       << "@.pipe.host.service.methods = private constant [" << descriptor.composition.size()
                       << " x %pipe.host.service.methods] [";
                for (std::size_t index = 0; index < descriptor.composition.size(); ++index) {
                    if (index != 0)
                        output << ", ";
                    const auto count = descriptor.composition[index].methods.size();
                    const auto table = count ? "@.pipe.host.plan." + std::to_string(index) + ".methods" : "null";
                    output << "%pipe.host.service.methods {ptr " << table << ", i64 " << count << '}';
                }
                output << "]\n";
            }
            const bool hasVariants = std::ranges::any_of(descriptor.composition,
                [](const HostedCompositionService& service) { return !service.variants.empty(); });
            if (hasVariants) {
                output << "%pipe.host.factory.variant = type {i64, i64, ptr, i64, ptr}\n";
                for (const auto& node : descriptor.composition) {
                    if (node.variants.empty())
                        continue;
                    const auto recipe = "@.pipe.host.recipe." + std::to_string(node.recipe - 1);
                    for (std::size_t index = 0; index < node.variants.size(); ++index) {
                        const auto prefix = recipe + ".variant." + std::to_string(index);
                        const auto& variant = node.variants[index];
                        if (!variant.methods.empty())
                            emitMethods(variant.methods, prefix, false);
                        if (!variant.cleanup.empty())
                            emitHostedCleanup(output, prefix, variant.cleanup);
                    }
                    output << recipe << ".variants = private constant [" << node.variants.size()
                           << " x %pipe.host.factory.variant] [";
                    for (std::size_t index = 0; index < node.variants.size(); ++index) {
                        if (index != 0)
                            output << ", ";
                        const auto prefix = recipe + ".variant." + std::to_string(index);
                        const auto& variant = node.variants[index];
                        output << "%pipe.host.factory.variant {i64 " << variant.receiverTag << ", i64 " << variant.receiverWords
                               << ", ptr " << (variant.methods.empty() ? "null" : prefix + ".methods")
                               << ", i64 " << variant.methods.size()
                               << ", ptr " << (variant.cleanup.empty() ? "null" : prefix + ".cleanup") << '}';
                    }
                    output << "]\n";
                }
                output << "%pipe.host.factory.variants = type {ptr, i64}\n"
                       << "@.pipe.host.variants = private constant [" << descriptor.recipes.size()
                       << " x %pipe.host.factory.variants] [";
                for (const auto& node : descriptor.composition) {
                    if (node.recipe == 0)
                        continue;
                    if (node.recipe != 1)
                        output << ", ";
                    output << "%pipe.host.factory.variants {ptr ";
                    if (node.variants.empty())
                        output << "null";
                    else
                        output << "@.pipe.host.recipe." << node.recipe - 1 << ".variants";
                    output << ", i64 " << node.variants.size() << '}';
                }
                output << "]\n";
            }
            if (descriptor.snapshot) {
                const auto& snapshot = *descriptor.snapshot;
                output << "@.pipe.host.snapshot.name = private unnamed_addr constant [" << snapshot.type.size() + 1
                       << " x i8] c\"" << llvmString(snapshot.type) << "\\00\"\n";
                const auto count = snapshot.fields.size();
                for (std::size_t index = 0; index < count; ++index) {
                    const auto& field = snapshot.fields[index];
                    output << "@.pipe.host.snapshot.field." << index << " = private unnamed_addr constant ["
                           << field.name.size() + 1 << " x i8] c\"" << llvmString(field.name) << "\\00\"\n";
                }
                if (count != 0) {
                    output << "@.pipe.host.snapshot.fields = private constant [" << count << " x %pipe.owned.field] [";
                    for (std::size_t index = 0; index < count; ++index) {
                        const auto& field = snapshot.fields[index];
                        if (index != 0)
                            output << ", ";
                        output << "%pipe.owned.field {i64 " << field.offset << ", ptr " << ownedShapeName(field.type)
                               << ", ptr @.pipe.host.snapshot.field." << index << '}';
                    }
                    output << "]\n";
                }
                output << "@.pipe.host.snapshot = private constant %pipe.owned {i64 "
                       << static_cast<std::uint64_t>(native_app_v19::OwnedKind::Record)
                       << ", i64 " << descriptor.receiverWords << ", i64 " << count << ", ptr "
                       << (count ? "@.pipe.host.snapshot.fields" : "null")
                       << ", ptr @.pipe.host.snapshot.name, i64 0, ptr null}\n";
            }
            const auto recipes = emitHostedComposition(output, descriptor);
            const bool hasOperations = recipes.factories || recipes.cleanups;
            emitHostedCompositionPlan(output, descriptor.composition);
            const auto argumentCount = descriptor.parameters.size();
            const auto bindings = emitHostedConstructor(output, "@.pipe.host", descriptor.constructor,
                                                        descriptor.parameters, descriptor.receiverTag);
            output << '@' << descriptor.symbol << " = constant {i64, ptr, ptr, i64, ptr, ptr, i64";
            const bool hasComposition = !descriptor.recipes.empty();
            const bool hasSnapshot = descriptor.snapshot.has_value();
            const bool hasMethodTable = hasMethods || hasSnapshot || hasComposition;
            const bool hasArgumentDescriptor = hasMethodTable || entryArguments.first != "null";
            if (hasArgumentDescriptor)
                output << ", ptr, i64";
            auto version = native_app_v19::hostedModelAbiVersion;
            if (hasMethodTable) {
                output << ", ptr, i64";
                version = native_app_v19::hostedModelWithMethodsAbiVersion;
            } else if (hasArgumentDescriptor) {
                version = native_app_v19::hostedModelWithArgumentsAbiVersion;
            }
            if (hasSnapshot || hasComposition) {
                output << ", ptr";
                version = native_app_v19::hostedModelWithSnapshotAbiVersion;
            }
            if (hasComposition) {
                output << ", ptr, i64";
                version = native_app_v19::hostedModelWithCompositionAbiVersion;
            }
            if (!descriptor.composition.empty()) {
                output << ", ptr, i64";
                version = native_app_v19::hostedModelWithCompositionPlanAbiVersion;
            }
            if (hasServiceMethods || hasOperations) {
                output << ", ptr";
                version = native_app_v19::hostedModelWithCompositionMethodsAbiVersion;
            }
            if (hasOperations) {
                output << ", ptr, ptr";
                version = native_app_v19::hostedModelWithCompositionOperationsAbiVersion;
            }
            if (hasVariants) {
                output << ", ptr";
                version = native_app_v19::hostedModelWithFactoryVariantsAbiVersion;
            }
            if (descriptor.retainsObjectGraphs)
                version |= native_app_v19::hostedModelGraphCapabilities;
            if (descriptor.allowsMultipleTasks)
                version |= native_app_v19::hostedModelMultipleTasksCapability;
            if (descriptor.exposesObjectReferences)
                version |= native_app_v19::hostedModelObjectReferencesCapability;
            if (sharedArguments)
                version |= native_app_v19::hostedModelSharedArgumentsCapability;
            if (collectionArguments)
                version |= native_app_v19::hostedModelCollectionArgumentsCapability;
            if (std::ranges::any_of(descriptor.parameters, [](const HostedParameter& parameter) {
                    return parameter.type != ScalarType::ContextOwnerReference &&
                        receiverSynchronizationKind(parameter.type).has_value();
                }))
                version |= native_app_v19::hostedModelSharedConstructorCapability;
            output << "} {i64 " << version << ", ptr @.pipe.host.construct, ptr "
                   << asyncDescriptor(entry.symbol) << ", i64 " << argumentCount << ", ptr " << payloadShape(entry)
                   << ", ptr " << bindings << ", i64 " << descriptor.receiverWords;
            if (hasArgumentDescriptor)
                output << ", ptr " << entryArguments.first << ", i64 " << entryArguments.second;
            if (hasMethodTable)
                output << ", ptr " << (hasMethods ? "@.pipe.host.methods" : "null") << ", i64 " << descriptor.methods.size();
            if (hasSnapshot || hasComposition)
                output << ", ptr " << (hasSnapshot ? "@.pipe.host.snapshot" : "null");
            if (hasComposition)
                output << ", ptr @.pipe.host.recipes, i64 " << descriptor.recipes.size();
            if (!descriptor.composition.empty())
                output << ", ptr @.pipe.host.plan, i64 " << descriptor.composition.size();
            if (hasServiceMethods || hasOperations)
                output << ", ptr " << (hasServiceMethods ? "@.pipe.host.service.methods" : "null");
            if (hasOperations) {
                output << ", ptr " << (recipes.factories ? "@.pipe.host.factories" : "null")
                       << ", ptr " << (recipes.cleanups ? "@.pipe.host.cleanups" : "null");
            }
            if (hasVariants)
                output << ", ptr @.pipe.host.variants";
            output << "}\n";
        }
    }

    std::vector<std::size_t> units;
    std::size_t metadata = 31;
    std::vector<std::size_t> layoutMetadata;
    for (const auto& layout : program.layouts) {
        layoutMetadata.push_back(metadata);
        metadata += 2 + (usesCollectionHandle(layout.kind) ? 2 : layout.fields.size() + layout.enumerators.size());
    }
    for (const auto& function : functions) {
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& fragment) {
            return fragment.path == function.sourceFile;
        });
        if (source == sources.end())
            throw std::logic_error("native function source was not admitted: " + function.sourceFile);

        units.push_back(metadata);
        const auto tracksSourceCallDepth = function.mode == FunctionMode::Construction
            ? program.tracksHostedSourceCallDepth : program.tracksSourceCallDepth;
        LlvmIrEmitter emitter(function, source->source, target, debug, metadata, program.layouts, layoutMetadata,
                             tracksSourceCallDepth, sourceTrace,
                             program.hosted && program.hosted->retainsObjectGraphs,
                             program.hosted && program.hosted->retainsObjectGraphs && program.hosted->allowsMultipleTasks,
                             activeHostFloating, program.hosted && !program.hosted->recipes.empty());
        output << emitter.emit() << '\n';
        metadata = emitter.nextMetadata();
    }
    if (program.numericExport)
        emitNumericExport(output, functions.back(), *program.numericExport, program.layouts);
    output << "!llvm.dbg.cu = !{";
    for (std::size_t index = 0; index < units.size(); ++index) {
        if (index != 0)
            output << ", ";
        output << '!' << units[index];
    }
    output << "}\n!llvm.module.flags = !{!0, !1}\n"
           << "!0 = !{i32 2, " << (target.format == ObjectFormat::Coff
                   ? "!\"CodeView\", i32 1" : "!\"Dwarf Version\", i32 4") << "}\n"
           << "!1 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
           << "!2 = !DIBasicType(name: \"int\", size: 64, encoding: DW_ATE_signed)\n"
           << "!3 = !DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)\n"
           << "!23 = !DIBasicType(name: \"uint32\", size: 32, encoding: DW_ATE_unsigned)\n"
           << "!24 = !DIBasicType(name: \"float\", size: 64, encoding: DW_ATE_float)\n"
           << "!25 = !DIBasicType(name: \"float32\", size: 32, encoding: DW_ATE_float)\n"
           << "!7 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)\n";
    if (functions.back().usesRuntime())
        output << "!8 = !DICompositeType(tag: DW_TAG_structure_type, name: \"string\", size: 128, elements: !10)\n"
               << "!10 = !{!12, !13}\n"
               << "!12 = !DIDerivedType(tag: DW_TAG_member, name: \"bytes\", scope: !8, baseType: !7, size: 64)\n"
               << "!13 = !DIDerivedType(tag: DW_TAG_member, name: \"size\", scope: !8, baseType: !17, size: 64, offset: 64)\n"
               << "!17 = !DIBasicType(name: \"uint64\", size: 64, encoding: DW_ATE_unsigned)\n"
               << "!18 = !DICompositeType(tag: DW_TAG_structure_type, name: \"Error\", size: 128, elements: !19)\n"
               << "!19 = !{!20, !21}\n"
               << "!20 = !DIDerivedType(tag: DW_TAG_member, name: \"bytes\", scope: !18, baseType: !7, size: 64)\n"
               << "!21 = !DIDerivedType(tag: DW_TAG_member, name: \"size\", scope: !18, baseType: !17, size: 64, offset: 64)\n"
               << "!22 = !DIDerivedType(tag: DW_TAG_pointer_type, name: \"CancellationToken\", baseType: null, size: 64)\n"
               << "!26 = !DICompositeType(tag: DW_TAG_structure_type, name: \"ByteBuffer\", size: 192, elements: !27)\n"
               << "!27 = !{!28, !29, !30}\n"
               << "!28 = !DIDerivedType(tag: DW_TAG_member, name: \"block\", scope: !26, baseType: !17, size: 64)\n"
               << "!29 = !DIDerivedType(tag: DW_TAG_member, name: \"offset\", scope: !26, baseType: !17, size: 64, offset: 64)\n"
               << "!30 = !DIDerivedType(tag: DW_TAG_member, name: \"size\", scope: !26, baseType: !17, size: 64, offset: 128)\n";

    for (std::size_t index = 0; index < program.layouts.size(); ++index) {
        const auto& layout = program.layouts[index];
        const auto id = layoutMetadata[index];
        if (layout.kind == ScalarType::FixedArray) {
            const auto element = layout.fields.front().type;
            output << '!' << id << " = !DICompositeType(tag: DW_TAG_array_type, name: \""
                   << llvmString(layout.name) << "\", baseType: !" << debugType(element, layoutMetadata)
                   << ", size: " << layout.words * 64 << ", elements: !" << id + 1 << ")\n"
                   << '!' << id + 1 << " = !{!" << id + 2 << "}\n"
                   << '!' << id + 2 << " = !DISubrange(count: " << layout.fixedExtent
                   << ", stride: " << valueWords(element, program.layouts) * 8 << ")\n";
            continue;
        }
        if (layout.kind == ScalarType::ClassReference) {
            output << '!' << id << " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !" << id + 1
                   << ", size: 64)\n"
                   << '!' << id + 1 << " = !DICompositeType(tag: DW_TAG_class_type, name: \""
                   << llvmString(layout.name) << "\", flags: DIFlagFwdDecl)\n";
            continue;
        }
        if (layout.kind == ScalarType::Enum) {
            output << '!' << id << " = !DICompositeType(tag: DW_TAG_enumeration_type, name: \""
                   << llvmString(layout.name) << "\", baseType: !2, size: 64, elements: !" << id + 1 << ")\n"
                   << '!' << id + 1 << " = !{";
            for (std::size_t entry = 0; entry < layout.enumerators.size(); ++entry) {
                if (entry != 0)
                    output << ", ";
                output << '!' << id + 2 + entry;
            }
            output << "}\n";
            for (std::size_t entry = 0; entry < layout.enumerators.size(); ++entry)
                output << '!' << id + 2 + entry << " = !DIEnumerator(name: \""
                       << llvmString(layout.enumerators[entry]) << "\", value: " << entry << ")\n";
            continue;
        }
        output << '!' << id << " = !DICompositeType(tag: DW_TAG_structure_type, name: \""
               << llvmString(layout.name) << "\", size: " << layout.words * 64 << ", elements: !" << id + 1 << ")\n"
               << '!' << id + 1 << " = !{";
        if (usesCollectionHandle(layout.kind)) {
            output << '!' << id + 2 << ", !" << id + 3 << "}\n"
                   << '!' << id + 2 << " = !DIDerivedType(tag: DW_TAG_member, name: \"storage\", scope: !" << id
                   << ", baseType: !17, size: 64)\n"
                   << '!' << id + 3 << " = !DIDerivedType(tag: DW_TAG_member, name: \"count\", scope: !" << id
                   << ", baseType: !17, size: 64, offset: 64)\n";
            continue;
        }
        for (std::size_t field = 0; field < layout.fields.size(); ++field) {
            if (field != 0)
                output << ", ";
            output << '!' << id + 2 + field;
        }
        output << "}\n";
        std::size_t offset = 0;
        for (std::size_t field = 0; field < layout.fields.size(); ++field) {
            const auto& member = layout.fields[field];
            const auto bits = valueWords(member.type, program.layouts) * 64;
            auto debugBits = bits;
            if (member.type == ScalarType::Boolean)
                debugBits = 8;
            else if (member.type == ScalarType::UInt32 || member.type == ScalarType::Float32)
                debugBits = 32;
            output << '!' << id + 2 + field << " = !DIDerivedType(tag: DW_TAG_member, name: \""
                   << llvmString(member.name) << "\", scope: !" << id
                   << ", baseType: !" << debugType(member.type, layoutMetadata)
                   << ", size: " << debugBits
                   << ", offset: " << offset << ")\n";
            offset += bits;
        }
    }

    return output.str();
}

}
