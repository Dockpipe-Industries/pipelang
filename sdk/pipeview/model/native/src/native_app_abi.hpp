// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pipelang::native_app_v19 {

enum class Collection : std::int32_t { IntegerArray = 3 };
enum class Mutation : std::int32_t { Set = 2 };
enum class Read : std::int32_t { At = 1, Count = 2 };
enum class ResultTag : std::int64_t { Invalid = 0, Success = 1, Failure = 2, Cancelled = 3 };
enum class IntegerOperation : std::int32_t { Add = 1, Subtract = 2, Multiply = 3, Divide = 4, Remainder = 5 };
enum class UInt32Operation : std::int32_t { Add, Subtract, Multiply, Divide, Remainder, ShiftLeft, ShiftRight, FromInt };
enum class ContextOwnerOperation : std::int32_t { Replace, Invalidate, Close };

enum class ReceiverSynchronizationKind : std::int32_t { Atomic, Lock, Semaphore, ConcurrentSequence, ConcurrentDictionary, ContextOwner };

enum class ParallelCaptureKind : std::int32_t { Owned, Atomic, Lock, Semaphore, ConcurrentSequence, ConcurrentDictionary };

enum class AtomicValueKind : std::int32_t { Integer, Boolean, Float, Float32 };
enum class AtomicOperation : std::int32_t { Load, Store, Exchange, CompareExchange, Add };
enum class FloatingBuiltin : std::int32_t {
    IsFinite, Abs, Sqrt, Log, Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Exp, Expm1, Pow, Hypot, Hypot3,
    Fmod, Remainder, CopySign, Cbrt, Tanh, Floor, Ceiling, Clamp, ILogB, ScaleB,
    FromInt, FromUInt32, ToInt, RoundToInt, ToUInt32, Widen, Narrow, ObserveEnvironment, RequireBinary32Environment
};
enum class TextOperation : std::int32_t {
    Concatenate, ByteLengthIs, ByteAtOr, StartsWith, FromInt, Utf8Length, Utf8Slice, Utf8IndexOf
};

struct OwnedShape;
class HostedValue;
enum class ConcurrentSequenceKind : std::int32_t { Queue, Stack };
enum class ConcurrentSequenceOperation : std::int32_t { Count, Append, TryTake, TryPeek, Clear, Items };
enum class ConcurrentDictionaryOperation : std::int32_t {
    Count, At, TryGet, Contains, TryAdd, Set, TryRemove, GetOrAdd, TryUpdate, Clear, Keys, Values, Entries
};

struct ParallelCapture {
    const OwnedShape* shape;
    ParallelCaptureKind kind;
};

struct TextView {
    const char* bytes;
    std::uint64_t size;
};

// An arena block uses a byte offset. Block zero borrows bytes from an owner
// whose lifetime extends through the invocation.
struct ByteView {
    std::uint64_t block;
    std::uint64_t offset;
    std::uint64_t size;
};
static_assert(sizeof(void*) == 8 && sizeof(ByteView) == 24);

enum class ByteOperation : std::int32_t { FromUtf8, At, Slice, IndexOf, DecodeUtf8, Append, Add };

struct HostArgument {
    // A null shape holds a pointer to a cancellation token.
    const OwnedShape* shape;
    const std::uint64_t* value;
};
static_assert(sizeof(HostArgument) == 16 && offsetof(HostArgument, value) == 8);

struct ListView {
    std::uint64_t block;
    std::uint64_t count;
};
static_assert(sizeof(ListView) == 16 && offsetof(ListView, count) == 8);

struct ArrayView {
    std::uint64_t version;
    std::uint64_t count;
};
static_assert(sizeof(ArrayView) == 16);

enum class ArrayOperation : std::int32_t { Create, At, Set };

struct SequenceView {
    std::uint64_t block;
    std::uint64_t count;
    std::uint64_t first;
};
static_assert(sizeof(SequenceView) == 24);

enum class SequenceOperation : std::int32_t { Append, Take, TryTake, Peek, At };

struct SetView {
    std::uint64_t version;
    std::uint64_t count;
};
static_assert(sizeof(SetView) == 16 && offsetof(SetView, count) == 8);

enum class SetOperation : std::int32_t { Contains, Add, Remove, ToList };
struct DictionaryView {
    std::uint64_t version;
    std::uint64_t count;
};
static_assert(sizeof(DictionaryView) == 16);

enum class DictionaryOperation : std::int32_t {
    At, TryGet, Contains, Add, Set, Remove, Keys, Values, Entries, Iterate
};

enum class OwnedKind : std::uint64_t {
    Integer, Boolean, Text, Error, Record, Result, List, Enum, Optional, Reference, HashSet, UInt32, Float, Float32,
    ByteBuffer, Queue, Stack, Dictionary, Pair, FixedArray, Array, ResultInterface, ObjectReference, ContextOwnerReference
};

struct ResultInterfaceHeader {
    std::uint64_t identity;
    std::uint64_t outcome;
    std::uint64_t ownedBytes;
    std::uint64_t ownedElements;
};
static_assert(sizeof(ResultInterfaceHeader) == 4 * sizeof(std::uint64_t));

struct OwnedField {
    std::uint64_t offsetWords;
    const OwnedShape* shape;
    const char* name;
};
struct OwnedShape {
    OwnedKind kind;
    std::uint64_t words;
    std::uint64_t fieldCount;
    const OwnedField* fields;
    const char* name;
    // Number of enum alternatives or elements in a fixed array.
    std::uint64_t valueCount;
    const char* const* enumNames;
};
static_assert(sizeof(OwnedField) == 24 && offsetof(OwnedField, shape) == 8 && offsetof(OwnedField, name) == 16);
inline bool hostedSemaphoreShape(const OwnedShape& shape)
{
    return shape.kind == OwnedKind::Reference && shape.words == 1 && shape.fieldCount == 0 &&
        shape.name && std::string_view(shape.name) == "SemaphoreSlim";
}

inline bool hostedIntegerQueueShape(const OwnedShape& shape)
{
    return shape.kind == OwnedKind::Reference && shape.words == 1 && shape.fieldCount == 0 &&
        shape.name && std::string_view(shape.name) == "ConcurrentQueue<int>";
}

inline bool hostedConcurrentSequenceShape(const OwnedShape& shape)
{
    if (shape.kind != OwnedKind::Reference || shape.words != 1 || shape.fieldCount != 0 || !shape.name)
        return false;
    const std::string_view type(shape.name);
    return type.ends_with('>') &&
        ((type.starts_with("ConcurrentQueue<") && type.size() > sizeof("ConcurrentQueue<>") - 1) ||
         (type.starts_with("ConcurrentStack<") && type.size() > sizeof("ConcurrentStack<>") - 1));
}

inline bool hostedConcurrentDictionaryShape(const OwnedShape& shape)
{
    if (shape.kind != OwnedKind::Reference || shape.words != 1 || shape.fieldCount != 0 || !shape.name)
        return false;
    const std::string_view type(shape.name);
    return type.starts_with("ConcurrentDictionary<") && type.ends_with('>') &&
        type.size() > sizeof("ConcurrentDictionary<,>") - 1;
}

inline bool hostedLockShape(const OwnedShape& shape)
{
    return shape.kind == OwnedKind::Reference && shape.words == 1 && shape.fieldCount == 0 &&
        shape.name && std::string_view(shape.name) == "Lock";
}

inline bool hostedAtomicShape(const OwnedShape& shape)
{
    if (shape.kind != OwnedKind::Reference || shape.words != 1 || shape.fieldCount != 0 || !shape.name)
        return false;
    const std::string_view name(shape.name);
    return name == "Atomic<int>" || name == "Atomic<bool>" || name == "Atomic<float>" || name == "Atomic<float32>";
}

static_assert(sizeof(OwnedShape) == 56 && offsetof(OwnedShape, words) == 8 &&
              offsetof(OwnedShape, fieldCount) == 16 && offsetof(OwnedShape, fields) == 24 &&
              offsetof(OwnedShape, name) == 32 && offsetof(OwnedShape, valueCount) == 40 &&
              offsetof(OwnedShape, enumNames) == 48);

struct ResultHeader {
    ResultTag tag;
    TextView error;
};

static_assert(sizeof(TextView) == 16 && offsetof(TextView, size) == 8);
static_assert(sizeof(ResultHeader) == 24 && offsetof(ResultHeader, error) == 8);
inline constexpr std::size_t resultHeaderWords = sizeof(ResultHeader) / sizeof(std::uint64_t);

inline constexpr std::size_t maximumSnapshotSites = 32;
inline constexpr std::size_t maximumSemaphoreSites = 32;
inline constexpr std::size_t maximumFrameWords = 8192;
inline constexpr std::size_t maximumArguments = 32;

// The receiver pointer addresses this header. Its fields start immediately after it.
inline constexpr std::size_t receiverSizeWord = 0;
inline constexpr std::size_t receiverTagWord = 1;
inline constexpr std::size_t receiverHeaderWords = 2;
inline constexpr std::size_t maximumReceiverWords = maximumFrameWords - receiverHeaderWords;

struct Argument {
    std::int64_t integer;
    // A token or an indirect argument lives in the caller's storage.
    const void* cancellation;
};
static_assert(sizeof(Argument) == 16 && offsetof(Argument, cancellation) == 8);

struct StepState;
using Resume = void (*)(void*, void*, const Argument*, std::int64_t, void*, std::uint64_t,
                        const void*, StepState*, void*);
struct AsyncFunction {
    Resume resume;
    std::uint64_t frameWords;
    std::uint64_t argumentCount;
    std::uint64_t cancellationArguments;
    std::uint64_t resultWords;
    std::uint64_t childResultWords;
};
static_assert(sizeof(AsyncFunction) == 48 && offsetof(AsyncFunction, frameWords) == 8 &&
              offsetof(AsyncFunction, argumentCount) == 16 && offsetof(AsyncFunction, cancellationArguments) == 24 &&
              offsetof(AsyncFunction, resultWords) == 32 && offsetof(AsyncFunction, childResultWords) == 40);

// Entry descriptors use reserved bits for runtime capabilities.
inline constexpr std::uint64_t sourceTraceCapability = std::uint64_t{1} << 63;
inline constexpr std::uint64_t processArgumentsCapability = std::uint64_t{1} << 62;
inline constexpr std::uint64_t synchronousCancellationCapability = std::uint64_t{1} << 61;
inline constexpr std::uint64_t portableBinary32PollCapability = std::uint64_t{1} << 60;
static_assert(maximumArguments < 60);

inline std::uint64_t cancellationArgumentMask(const AsyncFunction& function)
{
    return function.cancellationArguments &
        ~(sourceTraceCapability | processArgumentsCapability | synchronousCancellationCapability | portableBinary32PollCapability);
}

inline bool validAsyncFunction(const AsyncFunction& function)
{
    return function.resume && function.frameWords <= maximumFrameWords && function.argumentCount <= maximumArguments &&
        (cancellationArgumentMask(function) >> function.argumentCount) == 0 &&
        function.resultWords >= resultHeaderWords && function.resultWords <= maximumFrameWords - function.frameWords &&
        function.childResultWords <= maximumFrameWords - function.frameWords - function.resultWords;
}

inline constexpr std::uint64_t hostedModelAbiVersion = 9;
inline constexpr std::uint64_t hostedModelWithArgumentsAbiVersion = 10;
inline constexpr std::uint64_t hostedModelWithMethodsAbiVersion = 11;
inline constexpr std::uint64_t hostedModelWithSnapshotAbiVersion = 12;
inline constexpr std::uint64_t hostedModelWithCompositionAbiVersion = 13;
inline constexpr std::uint64_t hostedModelWithCompositionPlanAbiVersion = 14;
inline constexpr std::uint64_t hostedModelWithCompositionMethodsAbiVersion = 15;
inline constexpr std::uint64_t hostedModelWithCompositionOperationsAbiVersion = 16;
inline constexpr std::uint64_t hostedModelWithFactoryVariantsAbiVersion = 17;
inline constexpr std::uint64_t hostedModelGraphCapability = std::uint64_t{1} << 63;
inline constexpr std::uint64_t hostedModelPreciseRootsCapability = std::uint64_t{1} << 62;
inline constexpr std::uint64_t hostedModelMultipleTasksCapability = std::uint64_t{1} << 61;
inline constexpr std::uint64_t hostedModelObjectReferencesCapability = std::uint64_t{1} << 60;
inline constexpr std::uint64_t hostedModelSharedArgumentsCapability = std::uint64_t{1} << 59;
inline constexpr std::uint64_t hostedModelCollectionArgumentsCapability = std::uint64_t{1} << 58;
inline constexpr std::uint64_t hostedModelSharedConstructorCapability = std::uint64_t{1} << 57;
inline constexpr std::uint64_t hostedModelGraphCapabilities =
    hostedModelGraphCapability | hostedModelPreciseRootsCapability;
// Match the source compiler's per-class member limit for a complete export table.
inline constexpr std::size_t maximumHostedMethods = 256;
using ReceiverAllocation = void* (*)(void*, std::uint64_t, std::uint64_t) noexcept;
struct HostedParameter {
    const OwnedShape* value;
    const char* service;
};
static_assert(sizeof(HostedParameter) == 16 && offsetof(HostedParameter, service) == 8);

struct HostedModel {
    std::uint64_t version;
    std::int32_t (*construct)(void*, const std::uint64_t* const*, void*, ReceiverAllocation);
    const AsyncFunction* entry;
    std::uint64_t constructorArguments;
    const OwnedShape* successShape;
    const HostedParameter* parameters;
    std::uint64_t receiverWords;
};
static_assert(sizeof(HostedModel) == 56 && offsetof(HostedModel, construct) == 8 &&
              offsetof(HostedModel, entry) == 16 && offsetof(HostedModel, constructorArguments) == 24 &&
              offsetof(HostedModel, successShape) == 32 && offsetof(HostedModel, parameters) == 40 &&
              offsetof(HostedModel, receiverWords) == 48);

struct HostedModelWithArguments {
    HostedModel model;
    const OwnedShape* const* argumentShapes;
    std::uint64_t indirectArguments;
};
static_assert(sizeof(HostedModelWithArguments) == 72 && offsetof(HostedModelWithArguments, argumentShapes) == 56 &&
              offsetof(HostedModelWithArguments, indirectArguments) == 64);

struct HostedMethod {
    const char* name;
    const AsyncFunction* entry;
    const OwnedShape* successShape;
    const OwnedShape* const* argumentShapes;
    std::uint64_t indirectArguments;
};
static_assert(sizeof(HostedMethod) == 40 && offsetof(HostedMethod, entry) == 8 &&
              offsetof(HostedMethod, successShape) == 16 && offsetof(HostedMethod, argumentShapes) == 24 &&
              offsetof(HostedMethod, indirectArguments) == 32);

struct HostedModelWithMethods {
    HostedModelWithArguments model;
    const HostedMethod* methods;
    std::uint64_t methodCount;
};
static_assert(sizeof(HostedModelWithMethods) == 88 && offsetof(HostedModelWithMethods, methods) == 72 &&
              offsetof(HostedModelWithMethods, methodCount) == 80);

struct HostedModelWithSnapshot {
    HostedModelWithMethods model;
    const OwnedShape* snapshot;
};
static_assert(sizeof(HostedModelWithSnapshot) == 96 && offsetof(HostedModelWithSnapshot, snapshot) == 88);

struct HostedConstructorRecipe {
    const char* name;
    const OwnedShape* type;
    decltype(HostedModel::construct) construct;
    std::uint64_t receiverWords;
    std::uint64_t receiverTag;
    const HostedParameter* parameters;
    std::uint64_t parameterCount;
};
static_assert(sizeof(HostedConstructorRecipe) == 56);

struct HostedModelWithComposition {
    HostedModelWithSnapshot model;
    const HostedConstructorRecipe* recipes;
    std::uint64_t recipeCount;
};
static_assert(sizeof(HostedModelWithComposition) == 112 && offsetof(HostedModelWithComposition, recipes) == 96 &&
              offsetof(HostedModelWithComposition, recipeCount) == 104);

enum class HostedLifetime : std::uint64_t { Singleton, Scoped, Transient };
enum class HostedCompositionInput : std::uint64_t { None, Snapshot, Borrowed };
inline constexpr std::uint64_t hostedCompositionExported = 1;
inline constexpr std::uint64_t hostedCompositionRootResolvable = 2;
inline constexpr std::uint64_t hostedCompositionScopeResolvable = 4;

struct HostedCompositionNode {
    const char* name;
    HostedLifetime lifetime;
    HostedCompositionInput input;
    std::uint64_t flags;
    std::uint64_t recipe;
    const std::uint64_t* dependencies;
    std::uint64_t dependencyCount;
    const OwnedShape* snapshot;
};
static_assert(sizeof(HostedCompositionNode) == 64 && offsetof(HostedCompositionNode, snapshot) == 56);

struct HostedModelWithCompositionPlan {
    HostedModelWithComposition model;
    const HostedCompositionNode* nodes;
    std::uint64_t nodeCount;
};
static_assert(sizeof(HostedModelWithCompositionPlan) == 128 &&
    offsetof(HostedModelWithCompositionPlan, nodes) == 112 && offsetof(HostedModelWithCompositionPlan, nodeCount) == 120);

struct HostedCompositionMethods {
    const HostedMethod* methods;
    std::uint64_t methodCount;
};
static_assert(sizeof(HostedCompositionMethods) == 16);

struct HostedModelWithCompositionMethods {
    HostedModelWithCompositionPlan plan;
    const HostedCompositionMethods* tables;
};
static_assert(sizeof(HostedModelWithCompositionMethods) == 136 &&
    offsetof(HostedModelWithCompositionMethods, tables) == 128);

struct HostedFactoryResult {
    ResultHeader outcome;
    void* receiver;
};
static_assert(sizeof(HostedFactoryResult) == sizeof(ResultHeader) + sizeof(void*));

using HostedFactory = std::int32_t (*)(HostedFactoryResult*, const std::uint64_t* const*, void*, ReceiverAllocation, const void*);

using HostedCleanup = std::int32_t (*)(ResultHeader*, void*, void*, ReceiverAllocation, const void*);

struct HostedModelWithCompositionOperations {
    HostedModelWithCompositionMethods model;
    const HostedFactory* factories;
    const HostedCleanup* cleanups;
};
static_assert(sizeof(HostedModelWithCompositionOperations) == 152 &&
    offsetof(HostedModelWithCompositionOperations, factories) == 136 &&
    offsetof(HostedModelWithCompositionOperations, cleanups) == 144);

struct HostedFactoryVariant {
    std::uint64_t receiverTag;
    std::uint64_t receiverWords;
    HostedCompositionMethods methods;
    HostedCleanup cleanup;
};
static_assert(sizeof(HostedFactoryVariant) == 40 && offsetof(HostedFactoryVariant, cleanup) == 32);

struct HostedFactoryVariants {
    const HostedFactoryVariant* entries;
    std::uint64_t count;
};
static_assert(sizeof(HostedFactoryVariants) == 16);

struct HostedModelWithFactoryVariants {
    HostedModelWithCompositionOperations model;
    const HostedFactoryVariants* variants;
};
static_assert(sizeof(HostedModelWithFactoryVariants) == 160 &&
    offsetof(HostedModelWithFactoryVariants, variants) == 152);

struct StepState {
    std::uint64_t resumeBlock;
    std::int64_t semaphore;
    std::int64_t timeout;
    const AsyncFunction* callee;
    void* receiver;
    Argument arguments[maximumArguments];
    std::uint64_t faultBlock;
    const void* cancellation;
};
static_assert(sizeof(StepState) == 56 + maximumArguments * sizeof(Argument) && offsetof(StepState, semaphore) == 8 &&
              offsetof(StepState, timeout) == 16 && offsetof(StepState, callee) == 24 &&
              offsetof(StepState, receiver) == 32 && offsetof(StepState, arguments) == 40 &&
              offsetof(StepState, faultBlock) == 40 + maximumArguments * sizeof(Argument) &&
              offsetof(StepState, cancellation) == 48 + maximumArguments * sizeof(Argument));

}

extern "C" {
extern const pipelang::native_app_v19::AsyncFunction pipelang_native_v19_entry;
std::int32_t pipelang_native_v19_semaphore(void* frame, std::int64_t site, std::int64_t initial,
                                         std::int64_t maximum, std::int32_t hasMaximum) noexcept;
void pipelang_native_v19_invalid_result_value(void* frame) noexcept;
void pipelang_native_v19_invalid_optional_value(void* frame) noexcept;
std::int32_t pipelang_native_v19_step(void* frame) noexcept;
std::int32_t pipelang_native_v19_cancellation_requested(const void* token) noexcept;
std::int32_t pipelang_native_v19_begin_cleanup(void* frame) noexcept;
std::int32_t pipelang_native_v19_end_cleanup(void* frame) noexcept;
void pipelang_native_v19_route_fault(void* frame) noexcept;
std::int32_t pipelang_native_v19_capture_error(void* frame, pipelang::native_app_v19::TextView* output) noexcept;
std::int32_t pipelang_native_v19_import_process_arguments(void* frame,
    const pipelang::native_app_v19::OwnedShape* shape, const pipelang::native_app_v19::HostedValue* arguments,
    std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_raise_error(void* frame, const char* bytes, std::uint64_t size) noexcept;
std::int32_t pipelang_native_v19_hosted_raise_error(void* construction, const char* bytes, std::uint64_t size) noexcept;
std::int32_t pipelang_native_v19_hosted_invalid_result_value(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_invalid_dispatch(void* construction) noexcept;
void pipelang_native_v19_hosted_route_fault(void* construction, std::int32_t status) noexcept;
std::int32_t pipelang_native_v19_hosted_begin_cleanup(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_end_cleanup(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_begin_construction(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_begin_tracked_construction(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_enter_source_call(void* construction) noexcept;
void pipelang_native_v19_hosted_leave_source_call(void* construction) noexcept;
void pipelang_native_v19_hosted_finish_construction(void* construction, std::int32_t status) noexcept;
std::int32_t pipelang_native_v19_hosted_track_receiver(void* construction, void* receiver) noexcept;
std::int32_t pipelang_native_v19_hosted_list_append(void* construction, const void* list,
    const pipelang::native_app_v19::OwnedShape* element, const void* value, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_list_read(void* construction, const void* list,
    const pipelang::native_app_v19::OwnedShape* element, std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_sequence(void* construction, std::int32_t operation,
    const pipelang::native_app_v19::OwnedShape* shape, pipelang::native_app_v19::SequenceView* view,
    const void* argument, std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_dictionary(void* construction, std::int32_t operation,
    const pipelang::native_app_v19::OwnedShape* shape, pipelang::native_app_v19::DictionaryView* view,
    const void* key, const void* value, const pipelang::native_app_v19::OwnedShape* projection,
    std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_set_operation(void* construction, pipelang::native_app_v19::SetOperation operation,
    const void* set, const pipelang::native_app_v19::OwnedShape* key, const void* value, void* output,
    std::int64_t* changed) noexcept;
std::int32_t pipelang_native_v19_hosted_invalid_optional_value(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_invalid_fixed_array_index(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_validate_fixed_array(void* construction,
    const pipelang::native_app_v19::OwnedShape* shape, const void* value) noexcept;
std::int32_t pipelang_native_v19_hosted_require_result_interface(void* construction,
    std::uint64_t identity) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_atomic(void* construction, void* receiver,
    std::uint64_t index, std::int32_t kind, std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_atomic_call(void* construction, std::uint64_t handle,
    std::int32_t kind, std::int32_t operation, std::uint64_t value, std::uint64_t comparand,
    std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_concurrent_dictionary(void* construction, void* receiver,
    std::uint64_t index, const pipelang::native_app_v19::OwnedShape* key,
    const pipelang::native_app_v19::OwnedShape* value, std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_call_concurrent_dictionary(void* construction, std::uint64_t handle,
    std::int32_t operation, const pipelang::native_app_v19::OwnedShape* keyShape,
    const pipelang::native_app_v19::OwnedShape* valueShape, const std::uint64_t* key, const std::uint64_t* input,
    const std::uint64_t* comparison, const pipelang::native_app_v19::OwnedShape* result,
    std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_concurrent_sequence(void* construction, void* receiver,
    std::uint64_t index, std::int32_t kind, const pipelang::native_app_v19::OwnedShape* element,
    std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_call_concurrent_sequence(void* construction, std::uint64_t handle,
    std::int32_t operation, const pipelang::native_app_v19::OwnedShape* element, const std::uint64_t* input,
    const pipelang::native_app_v19::OwnedShape* result, std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_semaphore(void* construction, void* receiver,
    std::uint64_t index, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_semaphore_count(void* construction, std::int64_t handle,
    std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_release_semaphore(void* construction, std::int64_t handle,
    std::int64_t count, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_wait_semaphore(void* construction, std::int64_t handle,
    std::int64_t timeout, const void* token, std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_lock(void* construction, void* receiver,
    std::uint64_t index, std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_acquire_lock(void* construction, std::uint64_t handle) noexcept;
std::int32_t pipelang_native_v19_hosted_release_lock(void* construction) noexcept;
std::int32_t pipelang_native_v19_hosted_capture_error(void* construction,
    pipelang::native_app_v19::TextView* output) noexcept;
std::int32_t pipelang_native_v19_integer(void* frame, std::int32_t operation, std::int64_t left,
                                       std::int64_t right, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_uint32(void* frame, std::int32_t operation, std::int64_t left,
                                      std::int64_t right, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_uint32(void* construction, std::int32_t operation,
    std::int64_t left, std::int64_t right, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_floating(void* frame, std::int32_t operation, std::int32_t binary32,
                                        std::uint64_t first, std::uint64_t second, std::uint64_t third,
                                        std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_floating(void* construction, std::int32_t operation,
    std::int32_t binary32, std::uint64_t first, std::uint64_t second, std::uint64_t third,
    std::uint64_t* output) noexcept;
std::int32_t pipelang_native_v19_bytes(void* frame, std::int32_t operation, const void* receiver,
                                     const pipelang::native_app_v19::ByteView* argument, std::int64_t first,
                                     std::int64_t second, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_initialization_bytes(void* construction, std::int32_t operation,
    const void* receiver, const pipelang::native_app_v19::ByteView* argument, std::int64_t first,
    std::int64_t second, void* output) noexcept;
std::int32_t pipelang_native_v19_hosted_retain_receiver_bytes(void* construction, const void* receiver,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* words) noexcept;
std::int32_t pipelang_native_v19_hosted_record_byte_fields(void* construction,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* words) noexcept;
std::int32_t pipelang_native_v19_borrow_receiver_bytes(void* frame,
    const pipelang::native_app_v19::OwnedShape* shape, const std::uint64_t* words) noexcept;
std::int32_t pipelang_native_v19_write_receiver_bytes(void* frame, void* receiver, std::uint64_t field,
                                                     const pipelang::native_app_v19::ByteView* value) noexcept;
std::int32_t pipelang_native_v19_write_receiver_record(void* frame, void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* preparedValue) noexcept;
std::int32_t pipelang_native_v19_hosted_read_receiver_collections(void* construction, const void* receiver,
    std::uint64_t field, const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* value) noexcept;
std::int32_t pipelang_native_v19_hosted_write_receiver_collections(void* construction, void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* value) noexcept;
std::int32_t pipelang_native_v19_read_receiver_sets(void* frame, const void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* value) noexcept;
std::int32_t pipelang_native_v19_write_receiver_sets(void* frame, void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* preparedValue) noexcept;
std::int32_t pipelang_native_v19_read_receiver_values(void* frame, const void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* value) noexcept;
std::int32_t pipelang_native_v19_write_receiver_values(void* frame, void* receiver, std::uint64_t field,
    const pipelang::native_app_v19::OwnedShape* shape, std::uint64_t* preparedValue) noexcept;
std::int32_t pipelang_native_v19_snapshot(void* frame, std::int32_t site, void* collection, void** output) noexcept;
void pipelang_native_v19_release_snapshot(void* frame, std::int32_t site) noexcept;
std::int32_t pipelang_native_v19_create(void* frame, std::int32_t kind, std::int64_t size,
                                       std::int64_t initial, void** output) noexcept;
std::int32_t pipelang_native_v19_mutate(void* frame, void* collection, std::int32_t operation,
                                       std::int64_t index, std::int64_t value) noexcept;
std::int32_t pipelang_native_v19_read(void* frame, void* collection, std::int32_t operation,
                                     std::int64_t index, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_list_append(void* frame, const void* list, const pipelang::native_app_v19::OwnedShape* element,
                                           const void* value, void* output) noexcept;
std::int32_t pipelang_native_v19_list_read(void* frame, const void* list, const pipelang::native_app_v19::OwnedShape* element,
                                         std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_sequence(void* frame, std::int32_t operation,
    const pipelang::native_app_v19::OwnedShape* shape, pipelang::native_app_v19::SequenceView* view,
    const void* argument, std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_dictionary(void* frame, std::int32_t operation,
    const pipelang::native_app_v19::OwnedShape* shape, pipelang::native_app_v19::DictionaryView* view,
    const void* key, const void* value, const pipelang::native_app_v19::OwnedShape* projection,
    std::int64_t index, void* output) noexcept;
std::int32_t pipelang_native_v19_owned_equal(void* frame, const pipelang::native_app_v19::OwnedShape* shape,
                                           const void* left, const void* right, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_initialization_equal(void* construction,
    const pipelang::native_app_v19::OwnedShape* shape, const void* left, const void* right, std::int64_t* output) noexcept;
std::int32_t pipelang_native_v19_hosted_initialization_text(void* construction,
    pipelang::native_app_v19::TextOperation operation, const pipelang::native_app_v19::TextView* first,
    const pipelang::native_app_v19::TextView* second, std::int64_t index, std::int64_t extra, void* output) noexcept;
std::int32_t pipelang_native_v19_host_call(void* frame, std::uint64_t service, const char* interfaceName,
    const char* signature, const pipelang::native_app_v19::HostArgument* arguments, std::uint64_t count,
    const pipelang::native_app_v19::OwnedShape* expected, std::uint64_t fallible, void* output) noexcept;
std::int32_t pipelang_native_v19_host_call_with_reusable_bytes(void* frame, std::uint64_t service, const char* interfaceName,
    const char* signature, const pipelang::native_app_v19::HostArgument* arguments, std::uint64_t count,
    const pipelang::native_app_v19::OwnedShape* expected, std::uint64_t fallible, void* output,
    const void* byteSlot) noexcept;
std::int32_t pipelang_native_v19_prepare_host_call(void* frame, std::uint64_t service, const char* interfaceName,
    const char* signature, const pipelang::native_app_v19::HostArgument* arguments, std::uint64_t count,
    const pipelang::native_app_v19::OwnedShape* expected) noexcept;
}
