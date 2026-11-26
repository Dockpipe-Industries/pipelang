// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "native_app_execution.hpp"
#include "NativeSourceTrace.hpp"
#include "NativeLists.hpp"
#include "NativeInteger.hpp"
#include "NativeFloating.hpp"
#include "NativeAtomics.hpp"
#include "NativeConcurrentDictionaries.hpp"
#include "NativeConcurrentSequences.hpp"
#include "NativeHostedConstruction.hpp"
#include "NativeTextPool.hpp"
#include "native_host_projection.hpp"

#include <pipelang/model/native_host.hpp>
#include <pipelang/model/binary32.hpp>
#include <pipelang/model/byte_text.hpp>
#include <pipelang/model/cleanup.hpp>
#include <pipelang/model/extended_collections.hpp>
#include <pipelang/model/source_call.hpp>
#include <pipelang/model/synchronization.hpp>
#include <pipelang/model/uint32.hpp>

#include <algorithm>
#include "NativeParallel.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <deque>
#include <exception>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace pipelang::model::detail {

struct NativeTaskPollAccess {
    static void requirePortableBinary32(TaskScope& scope)
    {
        scope.setCurrentExecutionPollCheck(requireBinary32Environment);
    }
};

}

namespace {

using namespace pipelang::model;
using namespace pipelang::native_app_v19;
using OwnedCollection = Array<std::int64_t>;
using IntegerResult = Result<std::int64_t>;
using HostResult = Result<HostedServiceValue>;

struct PendingHostCall {
    std::shared_ptr<HostedService> service;
    std::shared_ptr<const ScopedServiceUse> scopedUse;
    std::string_view signature;
    std::vector<HostedServiceValue> arguments;
    const OwnedShape* expected;
};

class CleanupStack {
public:
    CleanupStack(TaskScope& scope, EvaluationBudget& budget, CancellationToken cancellation)
        : scope_(&scope), budget_(budget), cancellation_(std::move(cancellation))
    {
    }

    explicit CleanupStack(EvaluationBudget& budget)
        : scope_(nullptr), budget_(budget)
    {
    }

    ~CleanupStack()
    {
        if (regions_) {
            while (!regions_->empty())
                regions_->pop_back();
        }
    }

    const CancellationToken& cancellation() const { return cancellation_; }
    TaskScope& scope() const
    {
        if (!scope_)
            throw ExecutionFailure("parallel workers cannot suspend or invoke host services");
        return *scope_;
    }

    NativeContextOwners& contexts() noexcept { return contexts_; }

    void enter(const void* owner, std::exception_ptr primary)
    {
        AsyncFinallyFailure failure;
        failure.captureFailure(std::move(primary));
        try {
            if (!regions_)
                regions_.emplace();
            regions_->emplace_back(owner, scope_, budget_, cancellation_, failure);
        } catch (...) {
            failure.captureCleanupFailure(std::current_exception());
            failure.rethrowFailure();
        }
    }

    void leave(const void* owner, std::exception_ptr cleanupFailure)
    {
        if (!regions_ || regions_->empty() || regions_->back().owner != owner)
            throw ExecutionFailure("native cleanup exit has no matching entry");
        auto failure = std::move(regions_->back().failure);
        failure.captureCleanupFailure(std::move(cleanupFailure));
        regions_->pop_back();
        failure.rethrowFailure();
    }

private:
    struct TaskCleanup {
        TaskCleanup(TaskScope& scope, CancellationToken& cancellation)
            : guard(scope.enterCleanup(cancellation))
        {
        }

        TaskScope::CleanupRegion guard;
    };

    struct Region {
        Region(const void* frame, TaskScope* scope, EvaluationBudget& budget,
               CancellationToken& cancellation, AsyncFinallyFailure pending)
            : owner(frame), failure(std::move(pending)), evaluation(budget)
        {
            if (scope)
                task.emplace(*scope, cancellation);
        }

        const void* owner;
        AsyncFinallyFailure failure;
        EvaluationBudget::CleanupRegion evaluation;
        std::optional<TaskCleanup> task;
    };

    TaskScope* scope_;
    EvaluationBudget& budget_;
    CancellationToken cancellation_;
    std::optional<std::deque<Region>> regions_;
    NativeContextOwners contexts_;
};

class InvocationText;

struct InvocationConcurrentCollections {
    NativeConcurrentSequences sequences;
    NativeConcurrentDictionaries dictionaries;
};

struct RuntimeFrame {
    RuntimeFrame(EvaluationBudget& invocationBudget, CleanupStack& invocationCleanup, InvocationText& invocationText,
                 NativeLists& invocationLists, NativeReceivers& invocationReceivers,
                 InvocationSemaphores& invocationSemaphores, InvocationAtomics& invocationAtomics,
                 InvocationLocks& invocationLocks, InvocationConcurrentCollections& invocationCollections,
                 BoundServices invocationServices,
                 std::size_t invocationSourceDepth, NativeReceiverStorage* graph = nullptr)
        : budget(invocationBudget), cleanup(invocationCleanup), textStorage(invocationText), lists(invocationLists),
          receivers(invocationReceivers), sharedSemaphores(invocationSemaphores), atomics(invocationAtomics),
          locks(invocationLocks), concurrentCollections(invocationCollections), services(invocationServices),
          sourceDepth(invocationSourceDepth),
          segment(invocationReceivers, graph && graph->graphReceivers() ? graph : nullptr)
    {
    }

    std::optional<std::deque<OwnedCollection>> collections;
    std::array<std::optional<OwnedCollection>, maximumSnapshotSites> snapshots;
    std::array<std::optional<SemaphoreSlim>, maximumSemaphoreSites> semaphores;
    std::array<std::int64_t, maximumSemaphoreSites> sharedSemaphoreIds{};
    LocalAtomics localAtomics;
    std::vector<Lock::Guard> lockGuards;
    std::exception_ptr failure;
    std::exception_ptr pendingFailure;
    SourceTraceFrame awaitSite;
    EvaluationBudget& budget;
    CleanupStack& cleanup;
    InvocationText& textStorage;
    NativeLists& lists;
    NativeReceivers& receivers;
    InvocationSemaphores& sharedSemaphores;
    InvocationAtomics& atomics;
    InvocationLocks& locks;
    InvocationConcurrentCollections& concurrentCollections;
    BoundServices services;
    std::size_t sourceDepth;
    ReceiverTransactions segment;
    std::optional<PendingHostCall> pendingHost;

    const SemaphoreSlim& semaphore(std::int64_t handle) const;

    OwnedCollection& collection(void* handle)
    {
        if (collections) {
            const auto found = std::ranges::find_if(*collections, [&](const OwnedCollection& item) {
                return &item == handle;
            });
            if (found != collections->end())
                return *found;
        }
        for (auto& snapshot : snapshots) {
            if (snapshot && &*snapshot == handle)
                return *snapshot;
        }

        throw ExecutionFailure("native collection does not belong to the current invocation");
    }
};

class InvocationText {
public:
    explicit InvocationText(NativeReceiverStorage* receiverStorage, SourceTrace* trace = nullptr)
        : hostValues_(receiverStorage ? receiverStorage->referenceDomain() : nullptr),
          receiverStorage_(receiverStorage), trace_(trace) {}

    TextView retainError(std::string message)
    {
        return errors_.retain(std::move(message));
    }

    TextView retainText(std::string value)
    {
        if (value.empty())
            return {};
        return strings_.retain(std::move(value));
    }

    HostedInput& hostValues() { return hostValues_; }
    SourceTrace* trace() const { return trace_; }
    NativeReceiverStorage* receiverStorage() const { return receiverStorage_; }
    NativeReceiverText* receiverText() const { return receiverStorage_ ? &receiverStorage_->text : nullptr; }
    ReceiverTextBorrows& receiverBorrows() { return receiverBorrows_; }

    ~InvocationText()
    {
        for (const auto& [receiver, identity] : pinned_)
            receiverStorage_->releaseReceiver(receiver, identity);
    }

    void pinReceiver(const void* receiver)
    {
        if (!receiverStorage_ || !receiverStorage_->permitsMultipleTasks)
            return;
        const auto* graph = receiverStorage_->graphReceivers();
        if (!graph)
            return;
        const auto identity = graph->identity(receiver);
        if (identity == 0)
            return;
        const auto found = pinned_.find(receiver);
        if (found != pinned_.end()) {
            if (found->second == identity)
                return;
            found->second = identity;
        } else {
            // Construction rollback can retire a pinned allocation before its
            // task exits. Keep the pin table within the live receiver limit.
            if (pinned_.size() == pipelang::model::ObjectDomain::maximumCells) {
                std::erase_if(pinned_, [&](const auto& entry) {
                    return graph->identity(entry.first) != entry.second;
                });
            }
            pinned_.emplace(receiver, identity);
        }
        receiverStorage_->pinReceiver(receiver, identity);
    }

    void readReceiverValue(const NativeReceiverStorage& owner, const void* receiver, NativeLists& lists,
                           const OwnedShape& shape,
                           const std::uint64_t* source, std::uint64_t* output)
    {
        const auto snapshot = owner.values.read(receiver, source);
        const auto cached = receiverValues_.find(source);
        if (cached != receiverValues_.end() && cached->second.matches(shape, snapshot)) {
            std::copy(cached->second.words.begin(), cached->second.words.end(), output);
            return;
        }

        if (snapshot) {
            for (const auto* reference : snapshot->references)
                pinReceiver(reference);
            hostValues_.pack(shape, snapshot->value, output, &lists);
        } else {
            for (const auto* reference : owner.initialReferences(receiver, source))
                pinReceiver(reference);
            HostedProjection constructorValues(owner.constructorCollections, owner.referenceDomain());
            hostValues_.pack(shape, constructorValues.copy(shape, source), output, &lists);
        }

        const auto oldWords = cached == receiverValues_.end() ? 0 : cached->second.words.size();
        if (shape.words > maximumFrameWords - (receiverValueWords_ - oldWords))
            return;
        CachedReceiverValue imported{&shape, snapshot, {output, output + shape.words}, ++receiverValueRevision_};
        receiverValues_.insert_or_assign(source, std::move(imported));
        receiverValueWords_ = receiverValueWords_ - oldWords + shape.words;
    }

    std::uint64_t receiverValueCheckpoint() const noexcept { return receiverValueRevision_; }

    void restoreReceiverValues(std::uint64_t checkpoint) noexcept
    {
        // Packed values imported inside this savepoint can refer to reclaimed arena storage.
        std::erase_if(receiverValues_, [&](const auto& entry) {
            if (entry.second.revision <= checkpoint)
                return false;
            receiverValueWords_ -= entry.second.words.size();
            return true;
        });
    }

private:
    struct CachedReceiverValue {
        const OwnedShape* shape;
        std::weak_ptr<const NativeReceiverValues::Snapshot> snapshot;
        std::vector<std::uint64_t> words;
        std::uint64_t revision;

        bool matches(const OwnedShape& expected, const NativeReceiverValues::Value& value) const
        {
            // Check ownership identity so an expired snapshot never matches a
            // newly allocated snapshot or an original constructor value.
            return shape == &expected && !snapshot.owner_before(value) && !value.owner_before(snapshot);
        }
    };

    // Returned text and caught errors can outlive their producing frames.
    NativeTextPool errors_{maximumListBytes, "native invocation error storage limit exceeded"};
    NativeTextPool strings_{32 * maximumStringBytes, "native invocation text storage limit exceeded"};
    HostedInput hostValues_;
    NativeReceiverStorage* receiverStorage_;
    SourceTrace* trace_;
    ReceiverTextBorrows receiverBorrows_;
    std::map<const std::uint64_t*, CachedReceiverValue> receiverValues_;
    std::size_t receiverValueWords_ = 0;
    std::uint64_t receiverValueRevision_ = 0;
    std::map<const void*, std::uint64_t> pinned_;
};

template<class Storage>
Storage& synchronizationStorage(const RuntimeFrame& frame, std::uint64_t& handle,
                                Storage& transient, Storage NativeReceiverStorage::* persistent)
{
    if ((handle & hostedSynchronizationTag) == 0)
        return transient;
    auto* owner = frame.textStorage.receiverStorage();
    if (!owner)
        throw ExecutionFailure("native synchronization handle lost its hosted receiver");
    handle &= ~hostedSynchronizationTag;
    return owner->*persistent;
}

const SemaphoreSlim& RuntimeFrame::semaphore(std::int64_t handle) const
{
    if ((static_cast<std::uint64_t>(handle) & hostedSynchronizationTag) != 0) {
        auto identity = static_cast<std::uint64_t>(handle);
        auto& storage = synchronizationStorage(*this, identity, sharedSemaphores,
                                              &NativeReceiverStorage::semaphores);
        return storage.at(static_cast<std::int64_t>(identity));
    }
    if (handle >= static_cast<std::int64_t>(maximumSemaphoreSites))
        return sharedSemaphores.at(handle);
    if (handle < 0 || !semaphores[static_cast<std::size_t>(handle)])
        throw ExecutionFailure("native suspension has no initialized semaphore");
    return *semaphores[static_cast<std::size_t>(handle)];
}

template<class Operation>
std::int32_t invokeOperation(void* opaque, Operation operation) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (frame.failure)
        return 1;
    try {
        operation(frame);
        return 0;
    } catch (...) {
        frame.failure = std::current_exception();
        return 1;
    }
}

using ParallelWorker = std::int32_t (*)(void*, const std::uint64_t*);

class SegmentWait {
public:
    SegmentWait(TaskScope::PollingAwaitable wait, EvaluationBudget& budget, std::uint64_t& waits)
        : wait_(std::move(wait)), budget_(budget), waits_(waits)
    {
    }

    bool await_ready() { return wait_.await_ready(); }

    bool await_suspend(std::coroutine_handle<> continuation)
    {
        suspended_ = wait_.await_suspend(continuation);
        if (suspended_)
            ++waits_;
        return suspended_;
    }

    bool await_resume()
    {
        if (suspended_)
            budget_.reset();
        return wait_.await_resume();
    }

private:
    TaskScope::PollingAwaitable wait_;
    EvaluationBudget& budget_;
    std::uint64_t& waits_;
    bool suspended_ = false;
};

struct CompiledFrame {
    CompiledFrame(const AsyncFunction& descriptor, const Argument* inputs, void* instance,
                  EvaluationBudget& budget, CleanupStack& cleanup, InvocationText& textStorage, NativeLists& lists,
                  NativeReceivers& receivers, InvocationSemaphores& semaphores, InvocationAtomics& atomics,
                  InvocationLocks& locks, InvocationConcurrentCollections& concurrent, BoundServices services,
                  SourceCallContext parent)
        : function(descriptor), receiver(instance), ancestry(parent.enter()),
          runtime(budget, cleanup, textStorage, lists, receivers, semaphores, atomics, locks, concurrent, services,
                  ancestry.depth(), textStorage.receiverStorage())
    {
        if (!validAsyncFunction(function))
            throw ExecutionFailure("invalid native coroutine descriptor");

        std::copy_n(inputs, function.argumentCount, arguments.begin());
        storage.resize(function.frameWords + function.resultWords + function.childResultWords);
    }

    std::uint64_t* resultStorage() { return storage.data() + function.frameWords; }
    std::uint64_t* childStorage() { return resultStorage() + function.resultWords; }

    ResultHeader resultHeader()
    {
        ResultHeader header;
        std::memcpy(&header, resultStorage(), sizeof(header));
        return header;
    }

    const AsyncFunction& function;
    void* receiver;
    SourceCallContext ancestry;
    std::array<Argument, maximumArguments> arguments;
    std::vector<std::uint64_t> storage;
    // Rollback can address lexical receivers in storage.
    RuntimeFrame runtime;
    std::uint64_t programCounter = 0;
    std::uint64_t callFaultBlock = 0;
    bool acquired = false;
};

class CompiledFrames {
public:
    ~CompiledFrames()
    {
        while (!empty())
            pop();
    }

    void push(const AsyncFunction& function, const Argument* arguments, void* receiver,
              EvaluationBudget& budget, CleanupStack& cleanup, InvocationText& textStorage, NativeLists& lists,
              NativeReceivers& receivers, InvocationSemaphores& semaphores, InvocationAtomics& atomics,
              InvocationLocks& locks, InvocationConcurrentCollections& concurrent, BoundServices services,
              SourceCallContext parent)
    {
        frames_.push_back(std::make_unique<CompiledFrame>(function, arguments, receiver, budget, cleanup,
            textStorage, lists, receivers, semaphores, atomics, locks, concurrent, services, parent));
    }

    bool empty() const { return frames_.empty(); }
    CompiledFrame& top() { return *frames_.back(); }
    CompiledFrame* parent() { return frames_.size() > 1 ? frames_[frames_.size() - 2].get() : nullptr; }
    void pop() { frames_.pop_back(); }

private:
    std::vector<std::unique_ptr<CompiledFrame>> frames_;
};

ResultHeader failedResult(std::exception_ptr failure, InvocationText& textStorage)
{
    try {
        std::rethrow_exception(failure);
    } catch (const EvaluationLimitFailure&) {
        throw;
    } catch (const OperationCancelled& error) {
        return {ResultTag::Cancelled, textStorage.retainError(safeErrorMessage(error))};
    } catch (const std::exception& error) {
        return {ResultTag::Failure, textStorage.retainError(safeErrorMessage(error))};
    } catch (...) {
        return {ResultTag::Failure, textStorage.retainError("unexpected task failure")};
    }
}

std::string copyError(TextView error)
{
    if (error.size > maximumStringBytes || (error.size != 0 && !error.bytes))
        throw ExecutionFailure("native invocation produced an invalid result payload");
    std::string message;
    if (error.size != 0)
        message.assign(error.bytes, error.size);
    validate(message);
    return message;
}

std::string_view borrowText(const TextView* text)
{
    if (!text || text->size > maximumStringBytes || (text->size != 0 && !text->bytes))
        throw ExecutionFailure("native text operation received an invalid operand");
    if (text->size == 0)
        return {};
    return {text->bytes, text->size};
}

PendingHostCall decodeHostCall(BoundServices services, const NativeLists& lists, std::uint64_t service, const char* interfaceName,
                              const char* signature, std::span<const HostArgument> arguments,
                              const OwnedShape* expected)
{
    // The owner can add a binding between source segments. Copy the selected
    // service before calling it or handing its work to the host thread.
    const auto bindings = services.current();
    const auto slot = static_cast<std::uint32_t>(service);
    const auto generation = static_cast<std::uint32_t>(service >> 32);
    if (slot == 0 || slot > bindings.size() || arguments.size() > maximumArguments)
        throw ExecutionFailure("native host call has an invalid service handle or argument count");
    const auto& binding = bindings[slot - 1];
    if (binding.generation != generation || !binding.service)
        throw ExecutionFailure("native hosted port has closed");
    auto bound = binding.service;
    if (bound->interfaceName() != interfaceName)
        throw ExecutionFailure("native host call service type does not match the checked interface");

    PendingHostCall call{std::move(bound), binding.use, signature, {}, expected};
    call.arguments.reserve(arguments.size());
    HostedProjection projection(lists);
    for (const auto& argument : arguments) {
        if (!argument.shape) {
            const CancellationToken* token;
            std::memcpy(&token, argument.value, sizeof(token));
            call.arguments.emplace_back(token ? *token : CancellationToken{});
            continue;
        }
        switch (argument.shape->kind) {
        case OwnedKind::Integer: {
            std::int64_t integer;
            std::memcpy(&integer, argument.value, sizeof(integer));
            call.arguments.emplace_back(integer);
            break;
        }
        case OwnedKind::Boolean:
            call.arguments.emplace_back(argument.value[0] != 0);
            break;
        case OwnedKind::Text: {
            TextView text;
            std::memcpy(&text, argument.value, sizeof(text));
            call.arguments.emplace_back(copyError(text));
            break;
        }
        default:
            call.arguments.emplace_back(projection.copy(*argument.shape, argument.value));
            break;
        }
    }
    return call;
}

template<class RetainText, class RetainError>
void encodeHostResult(HostedInput& values, NativeLists& lists, const HostResult& result, const OwnedShape* expected,
                      bool fallible, void* output, const RetainText& retainText, const RetainError& retainError,
                      const void* byteSlot = nullptr)
{
    auto* payload = static_cast<std::byte*>(output);
    if (fallible) {
        ResultHeader header{ResultTag::Success, {}};
        if (!result.is_success()) {
            header.tag = result.is_cancelled() ? ResultTag::Cancelled : ResultTag::Failure;
            validate(result.error());
            header.error = retainError(result.error());
        }
        std::memcpy(payload, &header, sizeof(header));
        payload += sizeof(header);
        if (header.tag != ResultTag::Success) {
            if (expected)
                std::memset(payload, 0, expected->words * sizeof(std::uint64_t));
            return;
        }
    } else if (!result.is_success()) {
        validate(result.error());
        if (result.is_cancelled())
            throw OperationCancelled(result.error());
        throw ExecutionFailure(result.error());
    }

    const auto& value = result.value();
    if (!expected) {
        if (const auto* owned = std::get_if<HostedValue>(&value); owned && owned->kind() == HostedValue::Kind::Void)
            return;
    } else if (expected->kind == OwnedKind::Integer) {
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            std::memcpy(payload, integer, sizeof(*integer));
            return;
        }
    } else if (expected->kind == OwnedKind::Boolean) {
        if (const auto* boolean = std::get_if<bool>(&value)) {
            const std::uint64_t word = *boolean;
            std::memcpy(payload, &word, sizeof(word));
            return;
        }
    } else if (expected->kind == OwnedKind::Text) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            validate(*text);
            const auto retained = retainText(*text);
            std::memcpy(payload, &retained, sizeof(retained));
            return;
        }
    } else if (const auto* owned = std::get_if<HostedValue>(&value)) {
        auto* words = reinterpret_cast<std::uint64_t*>(payload);
        std::fill_n(words, expected->words, 0);
        values.pack(*expected, *owned, words, &lists, byteSlot);
        return;
    }
    throw ExecutionFailure("native host service result does not match the checked return type");
}

void encodeHostResult(RuntimeFrame& frame, const HostResult& result, const OwnedShape* expected,
                      bool fallible, void* output, const void* byteSlot = nullptr)
{
    encodeHostResult(frame.textStorage.hostValues(), frame.lists, result, expected, fallible, output,
        [&](const std::string& text) { return frame.textStorage.retainText(text); },
        [&](const std::string& error) { return frame.textStorage.retainError(error); }, byteSlot);
}

std::int32_t invokeHostCall(void* opaque, std::uint64_t service, const char* interfaceName,
    const char* signature, const HostArgument* arguments, std::uint64_t count, const OwnedShape* expected,
    bool fallible, void* output, const void* byteSlot) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto call = decodeHostCall(frame.services, frame.lists, service, interfaceName, signature,
                                         {arguments, count}, expected);
        const auto result = frame.cleanup.scope().invokeSynchronousHost([&] {
            return call.service->invoke(call.signature, call.arguments);
        });
        encodeHostResult(frame, result, expected, fallible, output, byteSlot);
    });
}

}

namespace pipelang::native_app_v19 {

std::int32_t evaluateInInvocation(void* frame, FloatingEvaluation evaluate, FloatingBuiltin operation,
                                  bool binary32, std::uint64_t first, std::uint64_t second, std::uint64_t third,
                                  std::uint64_t* output) noexcept
{
    return invokeOperation(frame, [&](RuntimeFrame&) {
        *output = evaluate(operation, binary32, first, second, third);
    });
}

HostedSharedArgument borrowHostedArgument(const OwnedShape& shape, const HostedValue& value, HostedSharedCells cells)
{
    if (!shape.name || value.typeName() != shape.name)
        throw ExecutionFailure("native shared argument does not match the checked type");

    using Kind = ReceiverSynchronizationKind;
    if (hostedSemaphoreShape(shape))
        return {Kind::Semaphore, static_cast<std::uint64_t>(cells.semaphores.retain(value.semaphore()))};
    if (hostedLockShape(shape))
        return {Kind::Lock, cells.locks.borrow(value.lock())};
    if (hostedAtomicShape(shape))
        return {Kind::Atomic, cells.atomics.borrowCell(value.atomic())};
    if (hostedConcurrentDictionaryShape(shape))
        return {Kind::ConcurrentDictionary, cells.dictionaries.borrow(value)};
    if (hostedConcurrentSequenceShape(shape) && value.kind() == HostedValue::Kind::ConcurrentSequence)
        return {Kind::ConcurrentSequence, cells.sequences.borrow(value)};
    if (hostedIntegerQueueShape(shape))
        return {Kind::ConcurrentSequence, cells.sequences.borrow(value.concurrentQueue())};

    throw ExecutionFailure("native shared argument has an unsupported shape");
}

model::Coroutine<model::Result<void>> runNative(
    model::TaskScope& scope, const AsyncFunction& function, void* receiver, const Argument* arguments,
    model::CancellationToken cancellation, std::uint64_t& waits, ReturnProjection project, void* output,
    BoundServices services, NativeReceiverStorage* receiverStorage, std::span<const InvocationValue> invocationValues,
    SourceTrace* trace)
{
    if (cancellation.is_cancelled())
        co_return InvocationResult::Cancelled("native invocation cancelled");
    if ((function.cancellationArguments & portableBinary32PollCapability) != 0)
        model::detail::NativeTaskPollAccess::requirePortableBinary32(scope);
    const auto publication = currentPublicationGuard();
    auto budget = EvaluationBudget::forInvocation(scope.evaluationStepsPerSegment());
    // Source steps check cancellation after protected assignments and leave cleanup independent.
    if ((function.cancellationArguments & synchronousCancellationCapability) != 0) {
        budget.setCancellationCheck([&] {
            if (!cancellation.is_cancelled())
                return false;
            if (!publication.is_current())
                throw StaleContextCancelled();
            return true;
        });
    }
    InvocationText textStorage(receiverStorage, trace);

    // Frame addresses remain stable while the callee owns a pointer to its
    // receiver in an older frame. Only a pending SDK operation suspends the
    // coroutine. Source calls use this stack even when they finish immediately.
    CleanupStack cleanup(scope, budget, cancellation);
    NativeLists lists;
    NativeReceivers receivers;
    auto* graph = receiverStorage ? receiverStorage->graphReceivers() : nullptr;
    const auto preparePublication = [&] {
        if (graph)
            receiverStorage->collectGraph(false);
        if (!publication.is_current())
            throw StaleContextCancelled();
    };
    InvocationSemaphores semaphores;
    InvocationAtomics atomics;
    InvocationLocks locks;
    InvocationConcurrentCollections concurrent;
    std::array<Argument, maximumArguments> packedArguments{};
    std::vector<std::uint64_t> packedValues;
    if (!invocationValues.empty()) {
        std::size_t words = 0;
        for (const auto& input : invocationValues) {
            if (input.shape)
                words += input.shape->words;
        }
        packedValues.resize(words);
        std::copy_n(arguments, invocationValues.size(), packedArguments.begin());
        auto* storage = packedValues.data();
        try {
            for (std::size_t index = 0; index < invocationValues.size(); ++index) {
                const auto& input = invocationValues[index];
                if (!input.shape)
                    continue;
                if (input.shape->kind == OwnedKind::Reference) {
                    const auto borrowed = borrowHostedArgument(*input.shape, *input.value,
                        {semaphores, locks, atomics, concurrent.sequences, concurrent.dictionaries});
                    packedArguments[index].integer = static_cast<std::int64_t>(borrowed.handle);
                } else {
                    textStorage.hostValues().pack(*input.shape, *input.value, storage, &lists);
                    if (input.indirect)
                        packedArguments[index].cancellation = storage;
                    else
                        packedArguments[index].integer = std::bit_cast<std::int64_t>(storage[0]);
                }
                storage += input.shape->words;
            }
        } catch (const ExecutionFailure& error) {
            co_return InvocationResult::Failure(safeErrorMessage(error));
        }
        arguments = packedArguments.data();
    }
    CompiledFrames frames;
    frames.push(function, arguments, receiver, budget, cleanup, textStorage, lists, receivers, semaphores, atomics, locks,
                concurrent, services, SourceCallContext{});
    for (;;) {
        auto& active = frames.top();
        active.runtime.awaitSite = {};
        ResultHeader result{};
        std::exception_ptr terminal;
        StepState step{};
        bool sourceReturned = false;
        try {
            {
                struct SnapshotScope {
                    const ReceiverSegment** location;

                    ~SnapshotScope()
                    {
                        if (location)
                            *location = nullptr;
                    }
                } snapshot{receiverStorage && (receiverStorage->exposesSnapshot || receiverStorage->permitsMultipleTasks)
                    ? &receiverStorage->snapshotWrites : nullptr};
                if (snapshot.location)
                    *snapshot.location = &active.runtime.segment.writes();
                active.function.resume(&active.runtime, active.receiver, active.arguments.data(),
                                       active.acquired ? 1 : 0, active.storage.data(),
                                       active.programCounter, active.childStorage(), &step, active.resultStorage());
            }
            if (active.runtime.failure)
                std::rethrow_exception(active.runtime.failure);
            if (!active.runtime.lockGuards.empty())
                throw ExecutionFailure("native invocation left a source lock held");

            if (step.resumeBlock == 0) {
                result = active.resultHeader();
                if (result.tag != ResultTag::Success && result.tag != ResultTag::Failure &&
                    result.tag != ResultTag::Cancelled)
                    throw ExecutionFailure("native invocation did not produce a valid result");
                preparePublication();
                sourceReturned = true;
            } else if (active.runtime.pendingHost) {
                auto call = std::move(*active.runtime.pendingHost);
                active.runtime.pendingHost.reset();
                const auto* expected = call.expected;
                if (resultHeaderWords + (expected ? expected->words : 0) > active.function.childResultWords)
                    throw ExecutionFailure("native host result exceeds the caller's payload storage");

                auto hostToken = CancellationToken::combine(cleanup.cancellation(), currentPermit().cancellation());
                for (const auto& argument : call.arguments) {
                    if (const auto* token = std::get_if<CancellationToken>(&argument))
                        hostToken = CancellationToken::combine(std::move(hostToken), *token);
                }
                for (auto& argument : call.arguments) {
                    if (auto* token = std::get_if<CancellationToken>(&argument))
                        *token = hostToken;
                }
                std::function<HostResult()> callback = [call = std::move(call)] {
                    return call.service->invoke(call.signature, call.arguments);
                };
                auto operation = scope.host<HostResult>(std::move(callback), std::move(hostToken));
                const auto suspended = operation.suspensionState();
                preparePublication();
                active.runtime.segment.commit();
                auto completed = co_await std::move(operation);
                if (*suspended) {
                    ++waits;
                    budget.reset();
                }
                encodeHostResult(active.runtime, completed, expected, true, active.childStorage());
                active.programCounter = step.resumeBlock;
                continue;
            } else if (step.callee) {
                if (step.callee->resultWords > active.function.childResultWords)
                    throw ExecutionFailure("native child result exceeds the caller's payload storage");
                preparePublication();
                active.runtime.segment.commit();
                active.programCounter = step.resumeBlock;
                active.callFaultBlock = step.faultBlock;
                frames.push(*step.callee, step.arguments, step.receiver, budget, cleanup, textStorage, lists,
                            receivers, semaphores, atomics, locks, concurrent, services, active.ancestry);
                continue;
            } else {
                const auto& semaphore = active.runtime.semaphore(step.semaphore);
                auto cancellationForWait = cleanup.cancellation();
                if (step.cancellation) {
                    const auto& explicitToken = *static_cast<const CancellationToken*>(step.cancellation);
                    cancellationForWait = CancellationToken::combine(std::move(cancellationForWait), explicitToken);
                }
                SegmentWait wait(semaphore.waitAsync(scope, step.timeout, std::move(cancellationForWait)), budget, waits);
                preparePublication();
                active.runtime.segment.commit();
                active.acquired = co_await wait;
                active.programCounter = step.resumeBlock;
                continue;
            }
        } catch (...) {
            const auto failure = std::current_exception();
            if (step.faultBlock != 0) {
                active.runtime.failure = {};
                active.runtime.pendingFailure = failure;
                active.programCounter = step.faultBlock;
                continue;
            }
            if (trace)
                trace->record(failure, active.runtime.awaitSite);
            try {
                result = failedResult(failure, textStorage);
                if (trace)
                    result.error = trace->retain(result.error, failure);
            } catch (const EvaluationLimitFailure&) {
                terminal = std::current_exception();
            }
        }

        if (terminal) {
            frames.pop();
            while (!frames.empty() && frames.top().callFaultBlock == 0)
                frames.pop();
            if (frames.empty())
                std::rethrow_exception(terminal);

            auto& parent = frames.top();
            parent.runtime.pendingFailure = std::move(terminal);
            parent.programCounter = parent.callFaultBlock;
            continue;
        }

        if (auto* parent = frames.parent()) {
            if (sourceReturned)
                active.runtime.segment.commit();
            auto* destination = parent->childStorage();
            if (result.tag == ResultTag::Success) {
                std::copy_n(active.resultStorage(), active.function.resultWords, destination);
            } else {
                if (trace)
                    result.error = trace->withCaller(result.error, parent->runtime.awaitSite);
                // A failed child carries an error, never a stale payload from
                // a previous call that used this storage.
                std::fill_n(destination, active.function.resultWords, std::uint64_t{});
                std::memcpy(destination, &result, sizeof(result));
            }
            frames.pop();
        } else {
            try {
                if (result.tag == ResultTag::Success) {
                    project(output, active.resultStorage() + resultHeaderWords, lists);
                    active.runtime.segment.commit();
                    // Publication succeeded. The hosted invocation can reuse
                    // the validated marks once its final frame has exited.
                    if (graph)
                        receiverStorage->preserveFinalGraphMark();
                    co_return InvocationResult::Success();
                }
                auto message = copyError(result.error);
                if (trace)
                    trace->publish(result.error);
                if (sourceReturned) {
                    active.runtime.segment.commit();
                    if (graph)
                        receiverStorage->preserveFinalGraphMark();
                }
                if (result.tag == ResultTag::Cancelled)
                    co_return InvocationResult::Cancelled(std::move(message));
                co_return InvocationResult::Failure(std::move(message));
            } catch (const ExecutionFailure& error) {
                co_return InvocationResult::Failure(safeErrorMessage(error));
            }
        }
    }
}

}

extern "C" std::int32_t pipelang_native_v19_import_process_arguments(
    void* opaque, const OwnedShape* shape, const HostedValue* arguments, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        if (!shape || !arguments || shape->kind != OwnedKind::Array)
            throw ExecutionFailure("native process argument layout is invalid");
        frame.textStorage.hostValues().pack(*shape, *arguments, output, &frame.lists);
    });
}

extern "C" void pipelang_native_v19_invalid_result_value(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    try {
        (void)IntegerResult{}.value();
    } catch (...) {
        frame.failure = std::current_exception();
    }
}

extern "C" std::int32_t pipelang_native_v19_hosted_invalid_result_value(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction&) { (void)IntegerResult{}.value(); });
}

extern "C" std::int32_t pipelang_native_v19_hosted_invalid_optional_value(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction&) { (void)Optional<std::int64_t>{}.value(); });
}

extern "C" void pipelang_native_v19_invalid_optional_value(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    try {
        (void)Optional<std::int64_t>{}.value();
    } catch (...) {
        frame.failure = std::current_exception();
    }
}

extern "C" std::int32_t pipelang_native_v19_hosted_invalid_fixed_array_index(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction&) {
        (void)collections_detail::indexOf(0, 0);
    });
}

extern "C" void pipelang_native_v19_invalid_fixed_array_index(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    try {
        (void)pipelang::model::collections_detail::indexOf(0, 0);
    } catch (...) {
        frame.failure = std::current_exception();
    }
}

extern "C" void pipelang_native_v19_uninitialized_receiver(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    try {
        throw ExecutionFailure("object reference is uninitialized");
    } catch (...) {
        frame.failure = std::current_exception();
    }
}

extern "C" void pipelang_native_v19_uninitialized_context_owner(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    try {
        pipelang::model::validate(std::shared_ptr<pipelang::model::ContextOwner>{});
    } catch (...) {
        frame.failure = std::current_exception();
    }
}

extern "C" void* pipelang_native_v19_allocate_receiver(void* opaque, std::uint64_t words, std::uint64_t tag) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (auto* storage = frame.textStorage.receiverStorage()) {
        if (const auto* graph = storage->graphReceivers(); graph && !frame.receivers.canShareAllocation(*graph, words))
            return nullptr;
    }
    return frame.receivers.allocate(words, tag);
}

extern "C" std::int32_t pipelang_native_v19_begin_construction(void* opaque, std::int32_t tracksDepth) noexcept
{
    return invokeOperation(opaque, [tracksDepth](RuntimeFrame& frame) {
        if (tracksDepth)
            SourceCallContext::requireCapacity(frame.sourceDepth);
        frame.segment.beginConstruction(frame.lists, {
            .semaphoreCheckpoint = frame.sharedSemaphores.checkpoint(),
            .atomicCheckpoint = frame.atomics.checkpoint(),
            .lockCheckpoint = frame.locks.checkpoint(),
            .concurrentSequenceCheckpoint = frame.concurrentCollections.sequences.checkpoint(),
            .concurrentDictionaryCheckpoint = frame.concurrentCollections.dictionaries.checkpoint(),
            .contextCheckpoint = frame.cleanup.contexts().checkpoint(),
            .tracksDepth = tracksDepth != 0,
            .receiverValueCheckpoint = frame.textStorage.receiverValueCheckpoint()
        });
        if (tracksDepth)
            ++frame.sourceDepth;
    });
}

extern "C" void pipelang_native_v19_finish_construction(void* opaque, std::int32_t status) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    const auto exit = frame.segment.finishConstruction(status == 0);
    if (status != 0) {
        frame.textStorage.restoreReceiverValues(exit.receiverValueCheckpoint);
        frame.sharedSemaphores.restore(exit.semaphoreCheckpoint);
        frame.atomics.restore(exit.atomicCheckpoint);
        frame.locks.restore(exit.lockCheckpoint);
        frame.concurrentCollections.sequences.restore(exit.concurrentSequenceCheckpoint);
        frame.concurrentCollections.dictionaries.restore(exit.concurrentDictionaryCheckpoint);
        frame.cleanup.contexts().restore(exit.contextCheckpoint);
        const auto retired = InvocationSemaphores::handleFor(exit.semaphoreCheckpoint);
        for (auto& handle : frame.sharedSemaphoreIds) {
            if (handle >= retired)
                handle = 0;
        }
    }
    if (exit.tracksDepth)
        --frame.sourceDepth;
}

extern "C" void* pipelang_native_v19_allocate_constructed_receiver(void* opaque, std::uint64_t words,
                                                                   std::uint64_t tag) noexcept
{
    auto* receiver = pipelang_native_v19_allocate_receiver(opaque, words, tag);
    if (!receiver)
        pipelang_native_v19_finish_construction(opaque, 1);
    return receiver;
}

extern "C" void* pipelang_native_v19_allocate_constructed_graph_receiver(
    void* opaque, std::uint64_t words, std::uint64_t tag) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    auto* storage = frame.textStorage.receiverStorage();
    auto* receivers = storage ? storage->graphReceivers() : nullptr;
    if (!receivers)
        return pipelang_native_v19_allocate_constructed_receiver(opaque, words, tag);
    auto* receiver = receivers->canShareAllocation(frame.receivers, words) ? receivers->allocate(words, tag) : nullptr;
    if (receiver) {
        storage->invalidateGraphMark();
        try {
            frame.textStorage.pinReceiver(receiver);
            return receiver;
        } catch (...) {
            frame.failure = std::current_exception();
        }
    }
    pipelang_native_v19_finish_construction(opaque, 1);
    return nullptr;
}

extern "C" std::int32_t pipelang_native_v19_pin_receiver(void* opaque, const void* receiver) noexcept
{
    return invokeOperation(opaque, [receiver](RuntimeFrame& frame) { frame.textStorage.pinReceiver(receiver); });
}
extern "C" std::int32_t pipelang_native_v19_track_receiver(void* opaque, void* receiver) noexcept
{
    return invokeOperation(opaque, [receiver](RuntimeFrame& frame) { frame.segment.track(receiver); });
}

static NativeReceiverText::Value retainReceiverBytes(ReceiverTransactions& transactions, ReceiverTextBorrows& borrows,
                                                      const void* receiver, NativeReceiverText& owner,
                                                      const std::uint64_t* address, std::string_view source)
{
    auto& field = owner.field(address);
    owner.admit(field, source.size());
    NativeReceiverText::Value retained;
    if (!source.empty())
        retained = std::make_shared<const std::string>(source);
    borrows.retain(field.current());
    borrows.retain(retained);
    transactions.trackText(receiver, owner, field);
    owner.replace(field, retained);
    return retained;
}

static void retainConstructedBytes(NativeHostedConstruction& construction, const void* receiver,
                                  const OwnedShape& shape, std::uint64_t* words)
{
    if (shape.kind == OwnedKind::Record) {
        for (const auto& field : std::span(shape.fields, shape.fieldCount))
            retainConstructedBytes(construction, receiver, *field.shape, words + field.offsetWords);
        return;
    }
    if (shape.kind == OwnedKind::ByteBuffer) {
        ByteView source;
        std::memcpy(&source, words, sizeof(source));
        const auto bytes = construction.lists.bytes().view(source);
        const auto retained = retainReceiverBytes(construction.transactions, construction.textBorrows, receiver,
                                                  construction.storage.text, words, NativeBytes::asText(bytes));
        const auto stable = retained ? NativeBytes::borrowed(*retained) : ByteView{};
        std::memcpy(words, &stable, sizeof(stable));
        return;
    }
    if (shape.kind != OwnedKind::Text)
        return;

    TextView source;
    std::memcpy(&source, words, sizeof(source));
    const auto retained = retainReceiverBytes(construction.transactions, construction.textBorrows,
                                              receiver, construction.storage.text, words, borrowText(&source));
    const auto stable = retained ? TextView{retained->data(), retained->size()} : TextView{};
    std::memcpy(words, &stable, sizeof(stable));
}

extern "C" std::int32_t pipelang_native_v19_hosted_retain_receiver_bytes(
    void* context, const void* receiver, const OwnedShape* shape, std::uint64_t* words) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        if (!construction.storage.text.owns(receiver))
            throw ExecutionFailure("native constructor byte storage does not belong to its graph");
        retainConstructedBytes(construction, receiver, *shape, words);
    });
}

static void borrowReceiverBytes(ReceiverTextBorrows& borrows, const NativeReceiverText& owner,
                               const OwnedShape& shape, const std::uint64_t* words)
{
    if (shape.kind == OwnedKind::Record) {
        for (const auto& field : std::span(shape.fields, shape.fieldCount))
            borrowReceiverBytes(borrows, owner, *field.shape, words + field.offsetWords);
        return;
    }
    if (shape.kind != OwnedKind::Text && shape.kind != OwnedKind::ByteBuffer)
        return;

    if (const auto* field = owner.find(words))
        borrows.retain(field->current());
}

extern "C" std::int32_t pipelang_native_v19_borrow_receiver_bytes(
    void* context, const OwnedShape* shape, const std::uint64_t* words) noexcept
{
    return invokeOperation(context, [&](RuntimeFrame& frame) {
        if (const auto* owner = frame.textStorage.receiverText())
            borrowReceiverBytes(frame.textStorage.receiverBorrows(), *owner, *shape, words);
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_text(void* opaque, void* receiver, std::uint64_t index,
    const char* bytes, std::uint64_t size) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* storage = static_cast<std::uint64_t*>(receiver);
        const auto words = storage[receiverSizeWord];
        if (index > words || words - index < sizeof(TextView) / sizeof(std::uint64_t))
            throw ExecutionFailure("native receiver text assignment exceeds the checked field storage");

        auto* address = storage + receiverHeaderWords + index;
        TextView value{bytes, size};
        if (auto* owner = frame.textStorage.receiverText(); owner && owner->owns(receiver)) {
            TextView previous;
            std::memcpy(&previous, address, sizeof(previous));
            const auto source = borrowText(&value);
            if (borrowText(&previous) == source)
                return;

            const auto retained = retainReceiverBytes(frame.segment, frame.textStorage.receiverBorrows(),
                                                      receiver, *owner, address, source);
            value = retained ? TextView{retained->data(), retained->size()} : TextView{};
        }
        std::memcpy(address, &value, sizeof(value));
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_bytes(void* opaque, void* receiver,
                                                                std::uint64_t index, const ByteView* input) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* storage = static_cast<std::uint64_t*>(receiver);
        const auto words = storage[receiverSizeWord];
        if (index > words || words - index < sizeof(ByteView) / sizeof(std::uint64_t))
            throw ExecutionFailure("native receiver byte buffer assignment exceeds the checked field storage");
        auto* address = storage + receiverHeaderWords + index;
        auto value = *input;
        if (auto* owner = frame.textStorage.receiverText(); owner && owner->owns(receiver)) {
            ByteView previous;
            std::memcpy(&previous, address, sizeof(previous));
            const auto& bytes = frame.lists.bytes();
            const auto source = NativeBytes::asText(bytes.view(value));
            if (source == NativeBytes::asText(bytes.view(previous)))
                return;
            const auto retained = retainReceiverBytes(frame.segment, frame.textStorage.receiverBorrows(),
                                                      receiver, *owner, address, source);
            value = retained ? NativeBytes::borrowed(*retained) : ByteView{};
        }
        std::memcpy(address, &value, sizeof(value));
    });
}

namespace {

std::string_view receiverRecordBytes(const std::uint64_t* words, OwnedKind kind, const NativeBytes& arena)
{
    if (kind == OwnedKind::Text) {
        TextView text;
        std::memcpy(&text, words, sizeof(text));
        return borrowText(&text);
    }
    ByteView bytes;
    std::memcpy(&bytes, words, sizeof(bytes));
    return NativeBytes::asText(arena.view(bytes));
}

void prepareReceiverRecord(const NativeBytes& arena, ReceiverTextBorrows& borrows, ReceiverTransactions& transaction,
                           const void* receiver, NativeReceiverText& owner, const OwnedShape& record,
                           std::uint64_t* storage, std::uint64_t* value)
{
    struct Field {
        const OwnedShape* shape;
        std::size_t offset;
    };
    struct Replacement {
        NativeReceiverText::Field* field;
        std::string_view bytes;
        Field location;
        NativeReceiverText::Value retained;
    };
    std::vector<Field> fields{{&record, 0}};
    std::vector<Replacement> replaced;
    std::size_t previousBytes = 0;
    std::size_t newBytes = 0;
    while (!fields.empty()) {
        const auto field = fields.back();
        fields.pop_back();
        const auto& shape = *field.shape;
        if (shape.kind == OwnedKind::Record) {
            for (const auto& child : std::span(shape.fields, shape.fieldCount)) {
                if (child.shape->kind == OwnedKind::Record || child.shape->kind == OwnedKind::Text ||
                    child.shape->kind == OwnedKind::ByteBuffer)
                    fields.push_back({child.shape, field.offset + child.offsetWords});
            }
            continue;
        }
        auto* destination = storage + field.offset;
        const auto source = receiverRecordBytes(value + field.offset, shape.kind, arena);
        if (receiverRecordBytes(destination, shape.kind, arena) == source) {
            std::copy_n(destination, shape.words, value + field.offset);
            continue;
        }
        auto& retention = owner.field(destination);
        if (source.size() > NativeReceiverText::maximumBytes - newBytes)
            throw EvaluationLimitFailure("native receiver text storage limit exceeded");
        newBytes += source.size();
        if (retention.current())
            previousBytes += retention.current()->size();
        replaced.push_back({&retention, source, field, {}});
    }

    owner.admitReplacement(previousBytes, newBytes);
    for (auto& replacement : replaced) {
        if (!replacement.bytes.empty())
            replacement.retained = std::make_shared<const std::string>(replacement.bytes);
        borrows.retain(replacement.field->current());
        borrows.retain(replacement.retained);
        transaction.trackText(receiver, owner, *replacement.field);
        auto* target = value + replacement.location.offset;
        if (replacement.location.shape->kind == OwnedKind::Text) {
            const auto text = replacement.retained ?
                TextView{replacement.retained->data(), replacement.retained->size()} : TextView{};
            std::memcpy(target, &text, sizeof(text));
        } else {
            const auto bytes = replacement.retained ? NativeBytes::borrowed(*replacement.retained) : ByteView{};
            std::memcpy(target, &bytes, sizeof(bytes));
        }
    }
    // The remaining writes cannot fail. The segment owns both the previous
    // words and their retained storage until the next commit.
    for (auto& replacement : replaced)
        owner.replace(*replacement.field, std::move(replacement.retained));
}

struct ValueReplacement {
    NativeReceiverValues::Field* field;
    NativeReceiverValues::Value value;
};

std::vector<ValueReplacement> prepareReceiverValues(const NativeLists& lists, ReceiverTransactions& transaction,
                                                      NativeReceiverStorage& receiverStorage,
                                                      const void* receiver, const OwnedShape& shape,
                                                      const std::uint64_t* storage,
                                                      std::uint64_t* value)
{
    auto& owner = receiverStorage.values;
    std::vector<ValueReplacement> replacements;
    std::size_t previousBytes = 0;
    std::size_t newBytes = 0;
    HostedProjection projection(lists, receiverStorage.referenceDomain());
    visitReceiverValues(shape, 0, [&](const OwnedShape& leaf, std::size_t offset) {
        const auto before = projection.bytes();
        auto owned = projection.copy(leaf, value + offset);
        const auto bytes = projection.bytes() - before;
        newBytes += bytes;
        auto& field = owner.field(receiver, storage + offset);
        if (field.current())
            previousBytes += field.current()->bytes;
        transaction.trackValue(receiver, owner, field);
        auto snapshot = std::make_shared<const NativeReceiverValues::Snapshot>(
            NativeReceiverValues::Snapshot{std::move(owned), bytes, projection.takeReferences()});
        replacements.push_back({&field, std::move(snapshot)});
        // The receiver keeps the snapshot, so its raw words must not point
        // into the invocation arena after publication.
        std::fill_n(value + offset, leaf.words, 0);
    });
    owner.admitReplacement(previousBytes, newBytes);
    return replacements;
}

void readReceiverSets(NativeLists& lists, const NativeReceiverSets& owner, const OwnedShape& shape,
                      const std::uint64_t* source, std::uint64_t* value)
{
    visitReceiverSets(shape, [&](const OwnedShape& key, std::size_t offset) {
        const auto view = lists.sets().retain(owner.read(source + offset, key), key);
        std::memcpy(value + offset, &view, sizeof(view));
    });
}

struct SetReplacement {
    NativeReceiverSets::Field* field;
    NativeSets::Snapshot value;
};

std::vector<SetReplacement> prepareReceiverSets(const NativeLists& lists, ReceiverTransactions& transaction,
                                                 const void* receiver, NativeReceiverSets& owner,
                                                 const OwnedShape& shape, const std::uint64_t* storage,
                                                 std::uint64_t* value)
{
    std::vector<SetReplacement> replacements;
    std::size_t previousBytes = 0;
    std::size_t newBytes = 0;
    visitReceiverSets(shape, [&](const OwnedShape& key, std::size_t offset) {
        SetView incoming;
        std::memcpy(&incoming, value + offset, sizeof(incoming));
        auto snapshot = lists.sets().snapshot(incoming, key);
        if (snapshot == owner.read(storage + offset, key)) {
            std::copy_n(storage + offset, sizeof(SetView) / sizeof(std::uint64_t), value + offset);
            return;
        }
        auto& field = owner.field(storage + offset);
        const auto bytes = NativeSets::snapshotBytes(snapshot);
        if (bytes > NativeReceiverSets::maximumBytes - newBytes)
            throw EvaluationLimitFailure("native receiver HashSet storage limit exceeded");
        newBytes += bytes;
        previousBytes += NativeSets::snapshotBytes(field.current().snapshot);
        transaction.trackSet(receiver, owner, field);
        replacements.push_back({&field, std::move(snapshot)});
        // The receiver owns the snapshot. Its word storage keeps the count
        // for debugging without retaining an invocation-local version ID.
        const SetView stored{0, incoming.count};
        std::memcpy(value + offset, &stored, sizeof(stored));
    });
    owner.admitReplacement(previousBytes, newBytes);
    return replacements;
}

template<class Word>
Word* receiverAggregate(Word* storage, std::uint64_t index, const OwnedShape& shape)
{
    if (!storage)
        throw ExecutionFailure("object reference is uninitialized");
    const auto words = storage[receiverSizeWord];
    if (index > words || shape.words > words - index)
        throw ExecutionFailure("native receiver aggregate exceeds the checked field storage");
    return storage + receiverHeaderWords + index;
}

}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_collections(
    void* context, const void* receiver, std::uint64_t index, const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        const auto* source = receiverAggregate(static_cast<const std::uint64_t*>(receiver), index, *shape);
        std::copy_n(source, shape->words, value);
        auto& owner = construction.storage;
        if (!owner.text.owns(receiver))
            return;

        HostedProjection initial(owner.constructorCollections, owner.referenceDomain());
        visitReceiverValues(*shape, 0, [&](const OwnedShape& leaf, std::size_t offset) {
            const auto retained = owner.values.read(receiver, source + offset);
            const auto snapshot = retained ? retained->value : initial.copy(leaf, source + offset);
            construction.values.pack(leaf, snapshot, value + offset, &construction.lists);
        });
        readReceiverSets(construction.lists, owner.sets, *shape, source, value);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_write_receiver_collections(
    void* context, void* receiver, std::uint64_t index, const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        construction.transactions.track(receiver);
        auto* destination = receiverAggregate(static_cast<std::uint64_t*>(receiver), index, *shape);
        auto& owner = construction.storage;
        if (owner.text.owns(receiver)) {
            auto values = prepareReceiverValues(construction.lists, construction.transactions, owner, receiver,
                                                *shape, destination, value);
            auto sets = prepareReceiverSets(construction.lists, construction.transactions, receiver, owner.sets,
                                            *shape, destination, value);
            if (shape->kind == OwnedKind::Record)
                prepareReceiverRecord(construction.lists.bytes(), construction.textBorrows, construction.transactions,
                                      receiver, owner.text, *shape, destination, value);
            for (auto& entry : sets)
                owner.sets.replace(*entry.field, {std::move(entry.value), true});
            if (!values.empty())
                owner.invalidateGraphMark();
            for (auto& entry : values)
                owner.values.replace(*entry.field, std::move(entry.value));
        }
        std::copy_n(value, shape->words, destination);
    });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_sets(void* opaque, const void* receiver, std::uint64_t index,
    const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto* source = receiverAggregate(static_cast<const std::uint64_t*>(receiver), index, *shape);
        std::copy_n(source, shape->words, value);
        if (const auto* owner = frame.textStorage.receiverStorage(); owner && owner->text.owns(receiver)) {
            readReceiverSets(frame.lists, owner->sets, *shape, source, value);
            if (owner->permitsMultipleTasks)
                borrowReceiverBytes(frame.textStorage.receiverBorrows(), owner->text, *shape, source);
        }
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_sets(void* opaque, void* receiver, std::uint64_t index,
    const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* destination = receiverAggregate(static_cast<std::uint64_t*>(receiver), index, *shape);
        if (auto* owner = frame.textStorage.receiverStorage(); owner && owner->text.owns(receiver)) {
            auto sets = prepareReceiverSets(frame.lists, frame.segment, receiver, owner->sets, *shape, destination, value);
            if (shape->kind == OwnedKind::Record)
                prepareReceiverRecord(frame.lists.bytes(), frame.textStorage.receiverBorrows(), frame.segment,
                                      receiver, owner->text, *shape, destination, value);
            for (auto& entry : sets)
                owner->sets.replace(*entry.field, {std::move(entry.value), true});
        }
        std::copy_n(value, shape->words, destination);
    });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_values(void* opaque, const void* receiver,
    std::uint64_t index, const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto* source = receiverAggregate(static_cast<const std::uint64_t*>(receiver), index, *shape);
        std::copy_n(source, shape->words, value);
        const auto* owner = frame.textStorage.receiverStorage();
        if (!owner || !owner->text.owns(receiver))
            return;

        visitReceiverValues(*shape, 0, [&](const OwnedShape& leaf, std::size_t offset) {
            frame.textStorage.readReceiverValue(*owner, receiver, frame.lists, leaf, source + offset, value + offset);
        });
        readReceiverSets(frame.lists, owner->sets, *shape, source, value);
        if (owner->permitsMultipleTasks)
            borrowReceiverBytes(frame.textStorage.receiverBorrows(), owner->text, *shape, source);
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_values(void* opaque, void* receiver,
    std::uint64_t index, const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* destination = receiverAggregate(static_cast<std::uint64_t*>(receiver), index, *shape);
        if (auto* owner = frame.textStorage.receiverStorage(); owner && owner->text.owns(receiver)) {
            auto values = prepareReceiverValues(frame.lists, frame.segment, *owner, receiver, *shape, destination, value);
            auto sets = prepareReceiverSets(frame.lists, frame.segment, receiver, owner->sets, *shape, destination, value);
            if (shape->kind == OwnedKind::Record)
                prepareReceiverRecord(frame.lists.bytes(), frame.textStorage.receiverBorrows(), frame.segment,
                                      receiver, owner->text, *shape, destination, value);
            for (auto& entry : sets)
                owner->sets.replace(*entry.field, {std::move(entry.value), true});
            if (!values.empty())
                owner->invalidateGraphMark();
            for (auto& entry : values)
                owner->values.replace(*entry.field, std::move(entry.value));
        }
        std::copy_n(value, shape->words, destination);
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_reference(
    void* opaque, void* receiver, std::uint64_t index, const void* reference) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* words = static_cast<std::uint64_t*>(receiver);
        if (index >= words[receiverSizeWord])
            throw ExecutionFailure("native receiver reference field is outside its layout");
        auto* field = words + receiverHeaderWords + index;
        if (auto* owner = frame.textStorage.receiverStorage(); owner && owner->text.owns(receiver)) {
            const auto* domain = owner->referenceDomain();
            if (!domain || !domain->owns(reference))
                throw ExecutionFailure("native receiver reference does not belong to its graph");
            owner->recordReferenceField(receiver, field);
        }
        *field = reinterpret_cast<std::uintptr_t>(reference);
    });
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_record(void* opaque, void* receiver, std::uint64_t index,
    const OwnedShape* shape, std::uint64_t* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto* storage = static_cast<std::uint64_t*>(receiver);
        const auto words = storage[receiverSizeWord];
        if (shape->kind != OwnedKind::Record || index > words || shape->words > words - index)
            throw ExecutionFailure("native receiver record assignment exceeds the checked field storage");
        auto* destination = storage + receiverHeaderWords + index;
        if (auto* owner = frame.textStorage.receiverText(); owner && owner->owns(receiver))
            prepareReceiverRecord(frame.lists.bytes(), frame.textStorage.receiverBorrows(), frame.segment,
                                  receiver, *owner, *shape, destination, value);
        std::copy_n(value, shape->words, destination);
    });
}

namespace {

template<class RetainText>
void applyByteOperation(NativeBytes& bytes, ByteOperation operation, const void* receiver, const ByteView* argument,
                        std::int64_t first, std::int64_t second, void* output, std::size_t protectedBlocks,
                        const RetainText& retain)
{
    if (operation == ByteOperation::FromUtf8) {
        const auto text = borrowText(static_cast<const TextView*>(receiver));
        validateText(text);
        *static_cast<ByteView*>(output) = NativeBytes::borrowed(text);
        return;
    }
    const auto value = *static_cast<const ByteView*>(receiver);
    switch (operation) {
    case ByteOperation::At:
        *static_cast<std::uint64_t*>(output) = bytes.at(value, first);
        return;
    case ByteOperation::Slice:
        *static_cast<ByteView*>(output) = bytes.slice(value, first, second);
        return;
    case ByteOperation::IndexOf:
        *static_cast<std::int64_t*>(output) = bytes.indexOf(value, *argument, first);
        return;
    case ByteOperation::DecodeUtf8: {
        const auto text = NativeBytes::asText(bytes.view(value));
        validateText(text);
        *static_cast<TextView*>(output) = retain(text);
        return;
    }
    case ByteOperation::Append:
        *static_cast<ByteView*>(output) = bytes.append(value, *argument, protectedBlocks);
        return;
    case ByteOperation::Add:
        *static_cast<ByteView*>(output) = bytes.add(value, checkedUInt32(first), protectedBlocks);
        return;
    default:
        throw ExecutionFailure("unknown native byte buffer operation");
    }
}

}

extern "C" std::int32_t pipelang_native_v19_bytes(void* opaque, std::int32_t operation,
                                                 const void* receiver, const ByteView* argument,
                                                 std::int64_t first, std::int64_t second, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        applyByteOperation(frame.lists.bytes(), static_cast<ByteOperation>(operation), receiver, argument, first,
                           second, output, frame.segment.protectedByteBlocks(), [&](std::string_view text) {
                               return frame.textStorage.retainText(std::string(text));
                           });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_initialization_bytes(void* opaque, std::int32_t operation,
    const void* receiver, const ByteView* argument, std::int64_t first, std::int64_t second, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        applyByteOperation(construction.lists.bytes(), static_cast<ByteOperation>(operation), receiver,
                           argument, first, second, output, construction.transactions.protectedByteBlocks(),
                           [&](std::string_view text) {
                               return construction.values.retainText(text);
                           });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_record_byte_fields(void* opaque, const OwnedShape* shape,
                                                                       std::uint64_t* words) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        construction.storage.recordConstructionByteFields(*shape, words);
    });
}

extern "C" std::int32_t pipelang_native_v19_construction_status(void* opaque, std::int32_t status) noexcept
{
    if (status == 0)
        return 0;
    return invokeOperation(opaque, [&](RuntimeFrame&) {
        if (status == 1)
            throw ExecutionFailure("native invocation receiver storage limit exceeded");
        throw ExecutionFailure("object reference is uninitialized");
    });
}

namespace {

[[noreturn]] void invalidDispatch()
{
    throw ExecutionFailure("receiver has no admitted implementation for the checked dispatch");
}

}

extern "C" void pipelang_native_v19_invalid_dispatch(void* opaque) noexcept
{
    invokeOperation(opaque, [](RuntimeFrame&) { invalidDispatch(); });
}

extern "C" std::int32_t pipelang_native_v19_hosted_invalid_dispatch(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction&) { invalidDispatch(); });
}

extern "C" std::int32_t pipelang_native_v19_semaphore(void* opaque, std::int64_t site, std::int64_t initial,
                                                      std::int64_t maximum, std::int32_t hasMaximum) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto semaphore = hasMaximum ? SemaphoreSlim::Create(initial, maximum) : SemaphoreSlim::Create(initial);
        frame.semaphores.at(static_cast<std::size_t>(site)).emplace(std::move(semaphore));
        frame.sharedSemaphoreIds[static_cast<std::size_t>(site)] = 0;
    });
}

extern "C" std::int32_t pipelang_native_v19_create_semaphore(void* opaque, std::int64_t initial,
                                                            std::int64_t maximum, std::int32_t hasMaximum,
                                                            std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.sharedSemaphores.retain(
            SemaphoreSlim::Create(initial, hasMaximum ? maximum : SemaphoreSlim::maximumCount));
    });
}

extern "C" std::int32_t pipelang_native_v19_share_semaphore(void* opaque, std::int64_t handle,
                                                           std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto& semaphore = frame.semaphore(handle);
        if (handle >= static_cast<std::int64_t>(maximumSemaphoreSites)) {
            *output = handle;
            return;
        }
        auto& shared = frame.sharedSemaphoreIds[static_cast<std::size_t>(handle)];
        if (shared == 0)
            shared = frame.sharedSemaphores.retain(semaphore);
        *output = shared;
    });
}

extern "C" std::int32_t pipelang_native_v19_semaphore_count(void* opaque, std::int64_t handle,
                                                           std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) { *output = frame.semaphore(handle).currentCount(); });
}

static std::uint64_t& synchronizationField(void* receiver, std::uint64_t index)
{
    if (!receiver)
        throw ExecutionFailure("object reference is uninitialized");
    auto* fields = static_cast<std::uint64_t*>(receiver);
    if (index >= fields[receiverSizeWord])
        throw ExecutionFailure("native synchronization read exceeds the checked receiver fields");
    return fields[receiverHeaderWords + index];
}

template<class Initialize>
static std::uint64_t readSynchronizationField(RuntimeFrame& frame, void* receiver, std::uint64_t index,
                                               const Initialize& initialize)
{
    auto& stored = synchronizationField(receiver, index);
    if (stored == 0) {
        frame.segment.track(receiver);
        stored = initialize();
    }
    return stored;
}

extern "C" std::int32_t pipelang_native_v19_write_receiver_synchronization(
    void* opaque, void* receiver, std::uint64_t index, std::int32_t rawKind, std::uint64_t handle) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.segment.track(receiver);
        auto& field = synchronizationField(receiver, index);
        auto* owner = frame.textStorage.receiverStorage();
        if (!owner || !owner->text.owns(receiver)) {
            field = handle;
            return;
        }

        const auto kind = static_cast<ReceiverSynchronizationKind>(rawKind);
        owner->recordSynchronizationField(receiver, &field, kind);
        if ((handle & hostedSynchronizationTag) != 0) {
            field = handle;
            return;
        }

        const auto retain = [&]() -> std::uint64_t {
            switch (kind) {
            case ReceiverSynchronizationKind::Atomic:
                if ((handle & LocalAtomics::handleTag) != 0)
                    return owner->atomics.borrowCell(frame.localAtomics.at(handle));
                return owner->atomics.borrowCell(frame.atomics, handle);
            case ReceiverSynchronizationKind::Lock:
                return owner->locks.borrow(frame.locks.at(handle));
            case ReceiverSynchronizationKind::Semaphore:
                return owner->semaphores.retain(frame.semaphore(handle));
            case ReceiverSynchronizationKind::ConcurrentSequence:
                return owner->sequences.borrow(frame.concurrentCollections.sequences, handle);
            case ReceiverSynchronizationKind::ConcurrentDictionary:
                return owner->dictionaries.borrow(frame.concurrentCollections.dictionaries, handle);
            case ReceiverSynchronizationKind::ContextOwner:
                return owner->contexts.retain(frame.cleanup.contexts().at(handle));
            }
            throw ExecutionFailure("invalid native receiver synchronization field");
        };
        field = owner->retainSynchronization(receiver, kind, retain());
    });
}

template<class Storage, class Initialize>
static std::uint64_t readHostedSynchronizationField(RuntimeFrame& frame, void* receiver, std::uint64_t index,
                                                    Storage& transient, Storage NativeReceiverStorage::* persistent,
                                                    ReceiverSynchronizationKind kind, const Initialize& initialize)
{
    if (auto* owner = frame.textStorage.receiverStorage(); owner && owner->text.owns(receiver)) {
        const auto& stored = synchronizationField(receiver, index);
        if (stored != 0)
            return stored;
        return owner->defaultSynchronization(receiver, &stored, kind, [&] { return initialize(owner->*persistent); });
    }
    return readSynchronizationField(frame, receiver, index, [&] { return initialize(transient); });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_semaphore(void* opaque, void* receiver,
                                                                   std::uint64_t index, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = static_cast<std::int64_t>(readHostedSynchronizationField(
            frame, receiver, index, frame.sharedSemaphores, &NativeReceiverStorage::semaphores,
            ReceiverSynchronizationKind::Semaphore, [](InvocationSemaphores& storage) { return static_cast<std::uint64_t>(storage.retain(SemaphoreSlim{})); }));
    });
}

extern "C" std::int32_t pipelang_native_v19_release_semaphore(void* opaque, std::int64_t handle,
                                                             std::int64_t count, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) { *output = frame.semaphore(handle).release(count); });
}

static void executeParallelOwned(
    RuntimeFrame& frame, const OwnedShape* sequence, const OwnedShape* item, const std::uint64_t* packed,
    ParallelWorker worker, std::uint64_t captureCount, const ParallelCapture* captureShapes,
    const CancellationToken* const* tokens, std::uint64_t tokenCount)
{
    if (!worker || !sequence || !item || !packed || captureCount >= maximumArguments ||
        (captureCount != 0 && !captureShapes) || item->words > maximumFrameWords ||
        tokenCount > maximumArguments || (tokenCount != 0 && !tokens))
        throw ExecutionFailure("invalid native parallel worker descriptor");

    auto cancellation = frame.cleanup.cancellation();
    for (std::size_t index = 0; index < tokenCount; ++index) {
        if (!tokens[index])
            throw ExecutionFailure("invalid native parallel cancellation token");
        cancellation = CancellationToken::combine(std::move(cancellation), *tokens[index]);
    }

    HostedProjection projection(frame.lists);
    const auto input = projection.copy(*sequence, packed);
    auto cursor = packed + sequence->words;
    std::size_t words = item->words;
    std::array<HostedValue, maximumArguments> captures;
    std::array<std::uint64_t, maximumArguments> shared{};
    for (std::size_t index = 0; index < captureCount; ++index) {
        const auto* shape = captureShapes[index].shape;
        if (!shape || shape->words > maximumFrameWords - words)
            throw ExecutionFailure("native parallel capture exceeds its storage limit");
        if (captureShapes[index].kind != ParallelCaptureKind::Owned)
            shared[index] = *cursor;
        else
            captures[index] = projection.copy(*shape, cursor);
        cursor += shape->words;
        words += shape->words;
    }

    const auto* binding = frame.textStorage.receiverStorage();
    const auto parallel = binding ? binding->parallelOptions() : ParallelOptions{};
    runNativeParallel(input, frame.budget, cancellation, parallel,
        [&](const HostedValue& element, EvaluationBudget& budget) {
            CleanupStack cleanup(budget);
            InvocationText textStorage(nullptr);
            NativeLists lists;
            NativeReceivers receivers;
            InvocationSemaphores semaphores;
            InvocationAtomics atomics;
            InvocationLocks locks;
            InvocationConcurrentCollections concurrent;

            // Shared SDK cells keep their identities. New allocations and
            // ordinary writable storage remain private to this worker.
            std::vector<std::uint64_t> arguments(words);
            HostedInput packing;
            packing.pack(*item, element, arguments.data(), &lists);
            auto offset = item->words;
            for (std::size_t index = 0; index < captureCount; ++index) {
                const auto& shape = *captureShapes[index].shape;
                switch (captureShapes[index].kind) {
                case ParallelCaptureKind::Owned:
                    packing.pack(shape, captures[index], arguments.data() + offset, &lists);
                    break;
                case ParallelCaptureKind::Atomic: {
                    auto handle = shared[index];
                    const auto& owner = synchronizationStorage(frame, handle, frame.atomics, &NativeReceiverStorage::atomics);
                    arguments[offset] = atomics.borrowCell(owner, handle);
                    break;
                }
                case ParallelCaptureKind::Lock: {
                    auto handle = shared[index];
                    const auto& owner = synchronizationStorage(frame, handle, frame.locks, &NativeReceiverStorage::locks);
                    arguments[offset] = locks.borrow(owner.at(handle));
                    break;
                }
                case ParallelCaptureKind::Semaphore:
                    arguments[offset] = static_cast<std::uint64_t>(
                        semaphores.retain(frame.semaphore(static_cast<std::int64_t>(shared[index]))));
                    break;
                case ParallelCaptureKind::ConcurrentSequence: {
                    auto handle = shared[index];
                    const auto& owner = synchronizationStorage(frame, handle, frame.concurrentCollections.sequences,
                                                               &NativeReceiverStorage::sequences);
                    arguments[offset] = concurrent.sequences.borrow(owner, handle);
                    break;
                }
                case ParallelCaptureKind::ConcurrentDictionary: {
                    auto handle = shared[index];
                    const auto& owner = synchronizationStorage(frame, handle, frame.concurrentCollections.dictionaries,
                                                               &NativeReceiverStorage::dictionaries);
                    arguments[offset] = concurrent.dictionaries.borrow(owner, handle);
                    break;
                }
                default:
                    throw ExecutionFailure("invalid native parallel capture kind");
                }
                offset += shape.words;
            }

            RuntimeFrame workerFrame(budget, cleanup, textStorage, lists, receivers, semaphores,
                                     atomics, locks, concurrent, {}, frame.sourceDepth);
            const auto status = worker(&workerFrame, arguments.data());
            if (workerFrame.failure)
                std::rethrow_exception(workerFrame.failure);
            if (status != 0 || workerFrame.pendingFailure || !workerFrame.lockGuards.empty())
                throw ExecutionFailure("native parallel worker did not complete cleanly");
        });
}

static std::int32_t parallelOwned(
    void* opaque, const OwnedShape* sequence, const OwnedShape* item, const std::uint64_t* packed,
    ParallelWorker worker, std::uint64_t captureCount, const ParallelCapture* captureShapes,
    const CancellationToken* const* tokens, std::uint64_t tokenCount) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        executeParallelOwned(frame, sequence, item, packed, worker, captureCount, captureShapes, tokens, tokenCount);
    });
}

extern "C" std::int32_t pipelang_native_v19_parallel_owned(
    void* opaque, const OwnedShape* sequence, const OwnedShape* item, const std::uint64_t* packed,
    ParallelWorker worker, std::uint64_t captureCount, const ParallelCapture* captureShapes) noexcept
{
    return parallelOwned(opaque, sequence, item, packed, worker, captureCount, captureShapes, nullptr, 0);
}

extern "C" std::int32_t pipelang_native_v19_parallel_owned_cancellable(
    void* opaque, const OwnedShape* sequence, const OwnedShape* item, const std::uint64_t* packed,
    ParallelWorker worker, std::uint64_t captureCount, const ParallelCapture* captureShapes,
    const CancellationToken* const* tokens, std::uint64_t tokenCount) noexcept
{
    return parallelOwned(opaque, sequence, item, packed, worker, captureCount, captureShapes, tokens, tokenCount);
}

extern "C" std::int32_t pipelang_native_v19_hosted_parallel_owned(
    void* opaque, const OwnedShape* sequence, const OwnedShape* item, const std::uint64_t* packed,
    ParallelWorker worker, std::uint64_t captureCount, const ParallelCapture* captureShapes,
    const CancellationToken* const* tokens, std::uint64_t tokenCount) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        // Constructor captures use cells retained by the binding. The transient tables stay empty.
        CleanupStack cleanup(construction.budget);
        InvocationText text(&construction.storage);
        NativeReceivers receivers;
        InvocationSemaphores semaphores;
        InvocationAtomics atomics;
        InvocationLocks locks;
        InvocationConcurrentCollections concurrent;
        RuntimeFrame frame(construction.budget, cleanup, text, construction.lists, receivers,
                           semaphores, atomics, locks, concurrent, {}, construction.sourceDepth);
        executeParallelOwned(frame, sequence, item, packed, worker, captureCount, captureShapes, tokens, tokenCount);
    });
}

extern "C" std::int32_t pipelang_native_v19_create_concurrent_dictionary(
    void* opaque, const OwnedShape* key, const OwnedShape* value, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.concurrentCollections.dictionaries.create(*key, *value);
    });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_concurrent_dictionary(
    void* opaque, void* receiver, std::uint64_t offset, const OwnedShape* key,
    const OwnedShape* value, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = readHostedSynchronizationField(
            frame, receiver, offset, frame.concurrentCollections.dictionaries, &NativeReceiverStorage::dictionaries,
            ReceiverSynchronizationKind::ConcurrentDictionary, [&](NativeConcurrentDictionaries& storage) {
                return storage.create(*key, *value);
            });
    });
}

extern "C" std::int32_t pipelang_native_v19_call_concurrent_dictionary(
    void* opaque, std::uint64_t handle, std::int32_t operation, const OwnedShape* keyShape,
    const OwnedShape* valueShape, const std::uint64_t* key, const std::uint64_t* input, const std::uint64_t* comparison,
    const OwnedShape* result, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto& storage = synchronizationStorage(frame, handle, frame.concurrentCollections.dictionaries,
                                               &NativeReceiverStorage::dictionaries);
        storage.call(handle, static_cast<ConcurrentDictionaryOperation>(operation),
            *keyShape, *valueShape, key, input, comparison, result, output, frame.lists, frame.textStorage.hostValues());
    });
}

extern "C" std::int32_t pipelang_native_v19_create_concurrent_sequence(
    void* opaque, std::int32_t kind, const OwnedShape* element, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.concurrentCollections.sequences.create(static_cast<ConcurrentSequenceKind>(kind), *element);
    });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_concurrent_sequence(
    void* opaque, void* receiver, std::uint64_t offset, std::int32_t kind,
    const OwnedShape* element, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = readHostedSynchronizationField(
            frame, receiver, offset, frame.concurrentCollections.sequences, &NativeReceiverStorage::sequences,
            ReceiverSynchronizationKind::ConcurrentSequence, [&](NativeConcurrentSequences& storage) {
                return storage.create(static_cast<ConcurrentSequenceKind>(kind), *element);
            });
    });
}

extern "C" std::int32_t pipelang_native_v19_call_concurrent_sequence(
    void* opaque, std::uint64_t handle, std::int32_t operation, const OwnedShape* element,
    const std::uint64_t* input, const OwnedShape* result, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto& storage = synchronizationStorage(frame, handle, frame.concurrentCollections.sequences,
                                               &NativeReceiverStorage::sequences);
        storage.call(handle, static_cast<ConcurrentSequenceOperation>(operation), *element,
                                      input, result, output, frame.lists, frame.textStorage.hostValues());
    });
}

namespace {

std::uint64_t hostedSynchronizationHandle(std::uint64_t handle)
{
    if ((handle & hostedSynchronizationTag) == 0)
        throw ExecutionFailure("native synchronization handle does not belong to the hosted binding");
    return handle & ~hostedSynchronizationTag;
}

const SemaphoreSlim& hostedSemaphore(NativeHostedConstruction& construction, std::int64_t handle)
{
    const auto identity = hostedSynchronizationHandle(static_cast<std::uint64_t>(handle));
    return construction.storage.semaphores.at(static_cast<std::int64_t>(identity));
}

template<class Initialize>
std::uint64_t readConstructionSynchronizationField(NativeHostedConstruction& construction, void* receiver,
    std::uint64_t index, ReceiverSynchronizationKind kind, const Initialize& initialize)
{
    auto& storage = construction.storage;
    if (!storage.text.owns(receiver))
        throw ExecutionFailure("native synchronization receiver does not belong to the hosted binding");

    const auto& stored = synchronizationField(receiver, index);
    return stored != 0 ? stored : storage.defaultSynchronization(receiver, &stored, kind, initialize);
}

}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_concurrent_dictionary(
    void* opaque, void* receiver, std::uint64_t index, const OwnedShape* key, const OwnedShape* value,
    std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = readConstructionSynchronizationField(construction, receiver, index,
            ReceiverSynchronizationKind::ConcurrentDictionary,
            [&] { return construction.storage.dictionaries.create(*key, *value); });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_call_concurrent_dictionary(
    void* opaque, std::uint64_t handle, std::int32_t operation, const OwnedShape* keyShape,
    const OwnedShape* valueShape, const std::uint64_t* key, const std::uint64_t* input,
    const std::uint64_t* comparison, const OwnedShape* result, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        construction.storage.dictionaries.call(hostedSynchronizationHandle(handle),
            static_cast<ConcurrentDictionaryOperation>(operation), *keyShape, *valueShape, key,
            input, comparison, result, output, construction.lists, construction.values);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_concurrent_sequence(
    void* opaque, void* receiver, std::uint64_t index, std::int32_t kind, const OwnedShape* element,
    std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = readConstructionSynchronizationField(construction, receiver, index,
            ReceiverSynchronizationKind::ConcurrentSequence, [&] {
                return construction.storage.sequences.create(static_cast<ConcurrentSequenceKind>(kind), *element);
            });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_call_concurrent_sequence(
    void* opaque, std::uint64_t handle, std::int32_t operation, const OwnedShape* element,
    const std::uint64_t* input, const OwnedShape* result, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        construction.storage.sequences.call(hostedSynchronizationHandle(handle),
            static_cast<ConcurrentSequenceOperation>(operation), *element, input, result, output,
            construction.lists, construction.values);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_semaphore(void* opaque, void* receiver,
    std::uint64_t index, std::int64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = static_cast<std::int64_t>(readConstructionSynchronizationField(
            construction, receiver, index, ReceiverSynchronizationKind::Semaphore,
            [&] { return static_cast<std::uint64_t>(construction.storage.semaphores.retain(SemaphoreSlim{})); }));
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_semaphore_count(void* opaque, std::int64_t handle,
    std::int64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = hostedSemaphore(construction, handle).currentCount();
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_release_semaphore(void* opaque, std::int64_t handle,
    std::int64_t count, std::int64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = hostedSemaphore(construction, handle).release(count);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_wait_semaphore(void* opaque, std::int64_t handle,
    std::int64_t timeout, const void* token, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        const auto cancellation = token ? *static_cast<const CancellationToken*>(token) : CancellationToken{};
        *output = hostedSemaphore(construction, handle).wait(construction.budget, timeout, cancellation);
    });
}

extern "C" std::int32_t pipelang_native_v19_wait_semaphore(void* opaque, std::int64_t handle,
                                                         std::int64_t timeout, const void* token,
                                                         std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto cancellation = frame.cleanup.cancellation();
        if (token)
            cancellation = CancellationToken::combine(std::move(cancellation),
                                                      *static_cast<const CancellationToken*>(token));
        *output = frame.semaphore(handle).wait(frame.budget, timeout, std::move(cancellation));
    });
}

extern "C" std::int32_t pipelang_native_v19_create_lock(void* opaque, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) { *output = frame.locks.create(); });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_lock(void* opaque, void* receiver, std::uint64_t index,
                                                              std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = readHostedSynchronizationField(frame, receiver, index, frame.locks, &NativeReceiverStorage::locks,
                                                ReceiverSynchronizationKind::Lock, [](InvocationLocks& storage) { return storage.create(); });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_lock(void* opaque, void* receiver,
    std::uint64_t index, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = readConstructionSynchronizationField(construction, receiver, index, ReceiverSynchronizationKind::Lock,
            [&] { return construction.storage.locks.create(); });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_acquire_lock(void* opaque, std::uint64_t handle) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        const auto& lock = construction.storage.locks.at(hostedSynchronizationHandle(handle));
        auto guard = lock.acquire(construction.budget, CancellationToken{});
        construction.lockGuards.push_back(std::move(guard));
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_release_lock(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction& construction) {
        if (construction.lockGuards.empty())
            throw ExecutionFailure("native lock is not held");
        construction.lockGuards.pop_back();
        if (auto failure = std::exchange(construction.pendingFailure, {}))
            std::rethrow_exception(failure);
    });
}

extern "C" std::int32_t pipelang_native_v19_acquire_lock(void* opaque, std::uint64_t handle) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto& storage = synchronizationStorage(frame, handle, frame.locks, &NativeReceiverStorage::locks);
        auto guard = storage.at(handle).acquire(frame.budget, frame.cleanup.cancellation());
        frame.lockGuards.push_back(std::move(guard));
    });
}

extern "C" std::int32_t pipelang_native_v19_release_lock(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (frame.lockGuards.empty())
        return invokeOperation(opaque, [](RuntimeFrame&) { throw ExecutionFailure("native lock is not held"); });
    frame.lockGuards.pop_back();
    // Fault routing parks the active exception while the guard is released.
    if (frame.pendingFailure)
        frame.failure = std::exchange(frame.pendingFailure, {});
    return frame.failure ? 1 : 0;
}

extern "C" std::int32_t pipelang_native_v19_create_atomic(void* opaque, std::int32_t kind,
                                                         std::uint64_t value, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.atomics.create(static_cast<AtomicValueKind>(kind), value);
    });
}

extern "C" std::int32_t pipelang_native_v19_create_local_atomic(void* opaque, std::uint64_t site, std::int32_t kind,
                                                               std::uint64_t value, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.localAtomics.initialize(site, static_cast<AtomicValueKind>(kind), value, frame.budget);
    });
}

extern "C" std::int32_t pipelang_native_v19_atomic_call(void* opaque, std::uint64_t handle, std::int32_t kind,
                                                       std::int32_t operation, std::uint64_t value,
                                                       std::uint64_t comparand, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto& storage = synchronizationStorage(frame, handle, frame.atomics, &NativeReceiverStorage::atomics);
        const auto& cell = (handle & LocalAtomics::handleTag) ? frame.localAtomics.at(handle) : storage.at(handle);
        *output = callAtomicCell(cell, static_cast<AtomicValueKind>(kind), static_cast<AtomicOperation>(operation),
                                 value, comparand, frame.budget);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_atomic_call(void* opaque, std::uint64_t handle,
    std::int32_t kind, std::int32_t operation, std::uint64_t value, std::uint64_t comparand, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        const auto& cell = construction.storage.atomics.at(hostedSynchronizationHandle(handle));
        *output = callAtomicCell(cell, static_cast<AtomicValueKind>(kind), static_cast<AtomicOperation>(operation),
                                 value, comparand, construction.budget);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_read_receiver_atomic(void* opaque, void* receiver,
    std::uint64_t index, std::int32_t kind, std::uint64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = readConstructionSynchronizationField(construction, receiver, index, ReceiverSynchronizationKind::Atomic,
            [&] { return construction.storage.atomics.create(static_cast<AtomicValueKind>(kind), 0); });
    });
}

extern "C" std::int32_t pipelang_native_v19_read_receiver_atomic(void* opaque, void* receiver, std::uint64_t index,
                                                                std::int32_t kind, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto valueKind = static_cast<AtomicValueKind>(kind);
        *output = readHostedSynchronizationField(frame, receiver, index, frame.atomics, &NativeReceiverStorage::atomics,
                                                ReceiverSynchronizationKind::Atomic, [&](InvocationAtomics& storage) { return storage.create(valueKind, 0); });
    });
}

extern "C" std::int32_t pipelang_native_v19_create_context_owner(void* opaque, std::uint64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto* binding = frame.textStorage.receiverStorage();
        auto& scope = binding ? binding->contextScope() : frame.cleanup.scope();
        *output = frame.cleanup.contexts().retain(scope.createContextOwner());
    });
}

extern "C" std::int32_t pipelang_native_v19_context_owner_call(
    void* opaque, std::uint64_t handle, std::int32_t operation) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        const auto& storage = synchronizationStorage(frame, handle, frame.cleanup.contexts(),
                                                     &NativeReceiverStorage::contexts);
        storage.apply(handle, static_cast<ContextOwnerOperation>(operation));
    });
}

extern "C" std::int32_t pipelang_native_v19_step(void* opaque) noexcept
{
    return invokeOperation(opaque, [](RuntimeFrame& frame) { frame.budget.step(); });
}

extern "C" std::int32_t pipelang_native_v19_enter_source_call(void* opaque) noexcept
{
    return invokeOperation(opaque, [](RuntimeFrame& frame) {
        SourceCallContext::requireCapacity(frame.sourceDepth);
        ++frame.sourceDepth;
    });
}

extern "C" void pipelang_native_v19_leave_source_call(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    --frame.sourceDepth;
}

extern "C" std::int32_t pipelang_native_v19_charge(void* opaque, std::uint64_t count) noexcept
{
    return invokeOperation(opaque, [count](RuntimeFrame& frame) { frame.budget.chargeSteps(count); });
}

extern "C" std::int32_t pipelang_native_v19_cancellation_requested(const void* token) noexcept
{
    return token && static_cast<const CancellationToken*>(token)->is_cancelled();
}

extern "C" std::int32_t pipelang_native_v19_begin_cleanup(void* opaque) noexcept
{
    return invokeOperation(opaque, [](RuntimeFrame& frame) {
        frame.cleanup.enter(&frame, std::exchange(frame.pendingFailure, {}));
    });
}

extern "C" std::int32_t pipelang_native_v19_end_cleanup(void* opaque) noexcept
{
    return invokeOperation(opaque, [](RuntimeFrame& frame) {
        frame.cleanup.leave(&frame, std::exchange(frame.pendingFailure, {}));
    });
}

extern "C" void pipelang_native_v19_route_fault(void* opaque) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    frame.pendingFailure = std::exchange(frame.failure, {});
}

extern "C" void pipelang_native_v19_note_failure(void* opaque, const char* function, const char* file,
                                                std::uint64_t line) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (auto* trace = frame.textStorage.trace(); trace && frame.failure)
        trace->record(frame.failure, {function, file, line});
}

extern "C" void pipelang_native_v19_note_await(void* opaque, const char* function, const char* file,
                                              std::uint64_t line) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (frame.textStorage.trace())
        frame.awaitSite = {function, file, line};
}

namespace {

std::string captureSourceError(std::exception_ptr failure)
{
    if (!failure)
        throw ExecutionFailure("native catch entered without a pending fault");
    try {
        std::rethrow_exception(failure);
    } catch (const EvaluationLimitFailure&) {
        throw;
    } catch (const ExecutionFailure& error) {
        return safeErrorMessage(error);
    }
}

}

extern "C" std::int32_t pipelang_native_v19_capture_error(void* opaque, TextView* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.textStorage.retainError(captureSourceError(std::exchange(frame.pendingFailure, {})));
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_capture_error(void* opaque, TextView* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = construction.values.retainText(captureSourceError(std::exchange(construction.pendingFailure, {})));
    });
}

extern "C" void pipelang_native_v19_hosted_route_fault(void* opaque, std::int32_t status) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(opaque);
    if (status == 3) {
        construction.pendingFailure = std::exchange(construction.failure, {});
        return;
    }
    try {
        if (status == 1)
            throw EvaluationLimitFailure("native hosted receiver allocation failed");
        if (status == 4)
            throw ExecutionFailure("context.null_owner");
        if (status == 5)
            (void)Optional<std::int64_t>{}.value();
        if (status == 6)
            (void)collections_detail::indexOf(0, 0);
        throw ExecutionFailure("object reference is uninitialized");
    } catch (...) {
        construction.pendingFailure = std::current_exception();
    }
}

extern "C" std::int32_t pipelang_native_v19_hosted_begin_cleanup(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction& construction) {
        AsyncFinallyFailure failure;
        failure.captureFailure(std::exchange(construction.pendingFailure, {}));
        try {
            construction.cleanup.push_back(
                std::make_unique<NativeHostedConstruction::CleanupRegion>(construction.budget, failure));
        } catch (...) {
            failure.captureCleanupFailure(std::current_exception());
            failure.rethrowFailure();
        }
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_end_cleanup(void* opaque) noexcept
{
    return withHostedConstruction(opaque, [](NativeHostedConstruction& construction) {
        if (construction.cleanup.empty())
            throw ExecutionFailure("native hosted cleanup exit has no matching entry");
        auto failure = std::move(construction.cleanup.back()->failure);
        failure.captureCleanupFailure(std::exchange(construction.pendingFailure, {}));
        construction.cleanup.pop_back();
        failure.rethrowFailure();
    });
}

namespace {

[[noreturn]] void raiseNativeError(const char* bytes, std::uint64_t size)
{
    if (size > maximumStringBytes || (size != 0 && !bytes))
        throw ExecutionFailure("native throw received an invalid error payload");
    std::string message;
    if (size != 0)
        message.assign(bytes, size);
    raiseError(Error{std::move(message)});
}

}

extern "C" std::int32_t pipelang_native_v19_raise_error(void* opaque, const char* bytes, std::uint64_t size) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame&) { raiseNativeError(bytes, size); });
}

extern "C" std::int32_t pipelang_native_v19_hosted_raise_error(void* opaque, const char* bytes, std::uint64_t size) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction&) { raiseNativeError(bytes, size); });
}

extern "C" std::int32_t pipelang_native_v19_integer(void* opaque, std::int32_t operation, std::int64_t left,
                                                    std::int64_t right, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame&) {
        *output = checkedInteger(static_cast<IntegerOperation>(operation), left, right);
    });
}

namespace {

std::uint32_t evaluateUInt32(UInt32Operation operation, std::int64_t left, std::int64_t right)
{
    const auto value = checkedUInt32(left);
    switch (operation) {
    case UInt32Operation::Add:
        return add(value, checkedUInt32(right));
    case UInt32Operation::Subtract:
        return subtract(value, checkedUInt32(right));
    case UInt32Operation::Multiply:
        return multiply(value, checkedUInt32(right));
    case UInt32Operation::Divide:
        return divide(value, checkedUInt32(right));
    case UInt32Operation::Remainder:
        return remainder(value, checkedUInt32(right));
    case UInt32Operation::ShiftLeft:
        return shiftLeft(value, right);
    case UInt32Operation::ShiftRight:
        return shiftRight(value, right);
    case UInt32Operation::FromInt:
        return value;
    }
    throw ExecutionFailure("unknown native uint32 operation");
}

}

extern "C" std::int32_t pipelang_native_v19_uint32(void* opaque, std::int32_t operation, std::int64_t left,
                                                   std::int64_t right, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame&) {
        *output = evaluateUInt32(static_cast<UInt32Operation>(operation), left, right);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_uint32(void* opaque, std::int32_t operation,
    std::int64_t left, std::int64_t right, std::int64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction&) {
        *output = evaluateUInt32(static_cast<UInt32Operation>(operation), left, right);
    });
}

extern "C" std::int32_t pipelang_native_v19_snapshot(void* opaque, std::int32_t site, void* handle,
                                                     void** output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto& snapshot = frame.snapshots.at(static_cast<std::size_t>(site));
        if (snapshot)
            throw ExecutionFailure("native collection snapshot site is already active");
        snapshot.emplace(frame.collection(handle));
        *output = &*snapshot;
    });
}

extern "C" void pipelang_native_v19_release_snapshot(void* opaque, std::int32_t site) noexcept
{
    auto& frame = *static_cast<RuntimeFrame*>(opaque);
    if (site >= 0 && static_cast<std::size_t>(site) < frame.snapshots.size())
        frame.snapshots[static_cast<std::size_t>(site)].reset();
}

extern "C" std::int32_t pipelang_native_v19_create(void* opaque, std::int32_t kind, std::int64_t size,
                                                  std::int64_t initial, void** output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        if (frame.collections && frame.collections->size() == 32)
            throw ExecutionFailure("native collection ownership limit exceeded");

        if (static_cast<Collection>(kind) != Collection::IntegerArray)
            throw ExecutionFailure("unknown native collection kind");
        auto& collections = frame.collections ? *frame.collections : frame.collections.emplace();
        collections.emplace_back(OwnedCollection::Create(size, initial));
        *output = &collections.back();
    });
}

extern "C" std::int32_t pipelang_native_v19_mutate(void* opaque, void* handle, std::int32_t operation,
                                                  std::int64_t index, std::int64_t value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        if (operation != static_cast<int>(Mutation::Set))
            throw ExecutionFailure("native mutation is unsupported for this collection");
        frame.collection(handle).set(index, value);
    });
}

extern "C" std::int32_t pipelang_native_v19_read(void* opaque, void* handle, std::int32_t operation,
                                                std::int64_t index, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        auto& collection = frame.collection(handle);
        if (operation == static_cast<int>(Read::At)) {
            *output = collection.at(index);
        } else if (operation == static_cast<int>(Read::Count)) {
            *output = collection.count();
        } else {
            throw ExecutionFailure("unknown native collection read");
        }
    });
}

namespace {

ListView loadListPrefix(const void* list)
{
    ListView prefix;
    std::memcpy(&prefix, list, sizeof(prefix));
    return prefix;
}

void appendList(NativeLists& lists, const void* list, const OwnedShape& element, const void* value,
                void* output, std::size_t protectedBlocks)
{
    const auto next = lists.append(loadListPrefix(list), element, value, protectedBlocks);
    std::memcpy(output, &next, sizeof(next));
}

}

extern "C" std::int32_t pipelang_native_v19_list_append(void* opaque, const void* list, const OwnedShape* element,
                                                       const void* value, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        appendList(frame.lists, list, *element, value, output, frame.segment.protectedListBlocks());
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_list_append(void* opaque, const void* list,
    const OwnedShape* element, const void* value, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        appendList(construction.lists, list, *element, value, output, construction.transactions.protectedListBlocks());
    });
}

extern "C" std::int32_t pipelang_native_v19_dictionary(void* opaque, std::int32_t operation, const OwnedShape* shape,
    DictionaryView* view, const void* key, const void* value, const OwnedShape* projection,
    std::int64_t index, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.lists.dictionaries().operate(frame.lists, static_cast<DictionaryOperation>(operation),
                                            *shape, *view, key, value, projection, index, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_dictionary(void* opaque, std::int32_t operation, const OwnedShape* shape,
    DictionaryView* view, const void* key, const void* value, const OwnedShape* projection,
    std::int64_t index, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        construction.lists.dictionaries().operate(construction.lists, static_cast<DictionaryOperation>(operation),
                                            *shape, *view, key, value, projection, index, output);
    });
}

static void applySequenceOperation(NativeLists& lists, std::int32_t operation, const OwnedShape* shape,
    SequenceView* view, const void* argument, std::int64_t index, void* output, std::size_t protectedBlocks)
{
    const auto op = static_cast<SequenceOperation>(operation);
    if (op == SequenceOperation::Append) {
        *view = lists.appendSequence(*view, *shape, argument, protectedBlocks);
        return;
    }
    const auto words = shape->fields[0].shape->words;
    if (op == SequenceOperation::TryTake && view->count == 0) {
        std::fill_n(static_cast<std::uint64_t*>(output), 1 + words, 0);
        return;
    }
    if (view->count == 0 && (op == SequenceOperation::Take ||
                            (op == SequenceOperation::Peek && shape->kind == OwnedKind::Stack)))
        throw ExecutionFailure("collection is empty");

    const auto item = lists.sequenceElement(*view, *shape, op == SequenceOperation::At ? index : 0);
    auto* target = static_cast<std::uint64_t*>(output);
    if (op == SequenceOperation::TryTake)
        *target++ = 1;
    std::memcpy(target, item.data(), item.size());
    if (op == SequenceOperation::Take || op == SequenceOperation::TryTake) {
        if (--view->count == 0)
            *view = {};
        else if (shape->kind == OwnedKind::Queue)
            ++view->first;
    }
}

extern "C" std::int32_t pipelang_native_v19_sequence(void* opaque, std::int32_t operation, const OwnedShape* shape,
    SequenceView* view, const void* argument, std::int64_t index, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        applySequenceOperation(frame.lists, operation, shape, view, argument, index, output,
                               frame.segment.protectedListBlocks());
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_sequence(void* opaque, std::int32_t operation, const OwnedShape* shape,
    SequenceView* view, const void* argument, std::int64_t index, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        applySequenceOperation(construction.lists, operation, shape, view, argument, index, output,
                               construction.transactions.protectedListBlocks());
    });
}

static void applyArrayOperation(NativeLists& lists, ArrayOperation operation, const OwnedShape& element,
                                ArrayView& view, std::int64_t index, const void* item, void* output)
{
    switch (operation) {
    case ArrayOperation::Create:
        view = lists.createArray(element, index, item);
        return;
    case ArrayOperation::At: {
        const auto value = lists.arrayElement(view, element, index);
        std::memcpy(output, value.data(), value.size());
        return;
    }
    case ArrayOperation::Set:
        view = lists.replaceArray(view, element, index, item);
        return;
    }
    throw ExecutionFailure("invalid native array operation");
}

extern "C" std::int32_t pipelang_native_v19_array(void* opaque, std::int32_t operation, const OwnedShape* shape,
    ArrayView* view, std::int64_t index, const void* item, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        applyArrayOperation(frame.lists, static_cast<ArrayOperation>(operation), *shape->fields[0].shape,
                            *view, index, item, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_initialization_array(void* opaque, std::int32_t operation,
    const OwnedShape* shape, ArrayView* view, std::int64_t index, const void* item, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        applyArrayOperation(construction.lists, static_cast<ArrayOperation>(operation), *shape->fields[0].shape,
                            *view, index, item, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_list_read(void* opaque, const void* list, const OwnedShape* element,
                                                     std::int64_t index, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        frame.lists.read(loadListPrefix(list), *element, index, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_list_read(void* opaque, const void* list,
    const OwnedShape* element, std::int64_t index, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        construction.lists.read(loadListPrefix(list), *element, index, output);
    });
}

static void applySetOperation(NativeLists& lists, SetOperation operation, const void* set,
    const OwnedShape* key, const void* value, void* output, std::int64_t* changed)
{
    SetView view;
    std::memcpy(&view, set, sizeof(view));
    auto& sets = lists.sets();
    switch (operation) {
    case SetOperation::Contains:
        *changed = sets.contains(view, *key, value);
        return;
    case SetOperation::Add:
    case SetOperation::Remove: {
        const auto result = sets.mutate(view, *key, value, operation == SetOperation::Add);
        std::memcpy(output, &result.view, sizeof(result.view));
        *changed = result.changed;
        return;
    }
    case SetOperation::ToList: {
        ListView items{};
        sets.visitValues(view, *key, [&](const void* item) {
            items = lists.append(items, *key, item);
        });
        std::memcpy(output, &items, sizeof(items));
        return;
    }
    }
    throw ExecutionFailure("invalid native HashSet operation");
}

extern "C" std::int32_t pipelang_native_v19_set_operation(void* opaque, SetOperation operation, const void* set,
    const OwnedShape* key, const void* value, void* output, std::int64_t* changed) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        applySetOperation(frame.lists, operation, set, key, value, output, changed);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_set_operation(void* opaque, SetOperation operation, const void* set,
    const OwnedShape* key, const void* value, void* output, std::int64_t* changed) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        applySetOperation(construction.lists, operation, set, key, value, output, changed);
    });
}

namespace {

template<class RetainText>
void applyTextOperation(TextOperation operation, const TextView* first, const TextView* second,
                        std::int64_t index, std::int64_t extra, void* output, const RetainText& retain)
{
    const auto store = [output](auto value) { std::memcpy(output, &value, sizeof(value)); };
    switch (operation) {
    case TextOperation::Concatenate:
        store(retain(concatenate(borrowText(first), borrowText(second))));
        return;
    case TextOperation::ByteLengthIs:
        store(static_cast<std::int64_t>(byteLengthIs(borrowText(first), index)));
        return;
    case TextOperation::ByteAtOr:
        store(byteAtOr(borrowText(first), index, extra));
        return;
    case TextOperation::StartsWith:
        store(static_cast<std::int64_t>(borrowText(first).starts_with(borrowText(second))));
        return;
    case TextOperation::FromInt:
        store(retain(stringFromInt(index)));
        return;
    case TextOperation::Utf8Length:
        store(utf8Length(borrowText(first)));
        return;
    case TextOperation::Utf8Slice: {
        const auto slice = utf8SliceView(borrowText(first), index, extra);
        store(TextView{slice.data(), slice.size()});
        return;
    }
    case TextOperation::Utf8IndexOf:
        store(utf8IndexOf(borrowText(first), borrowText(second), index));
        return;
    }
    throw ExecutionFailure("invalid native text operation");
}

}

extern "C" std::int32_t pipelang_native_v19_text_operation(void* opaque, TextOperation operation,
    const TextView* first, const TextView* second, std::int64_t index, std::int64_t extra, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        applyTextOperation(operation, first, second, index, extra, output,
            [&](std::string value) { return frame.textStorage.retainText(std::move(value)); });
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_initialization_text(void* opaque, TextOperation operation,
    const TextView* first, const TextView* second, std::int64_t index, std::int64_t extra, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        applyTextOperation(operation, first, second, index, extra, output,
            [&](std::string_view value) { return construction.values.retainText(value); });
    });
}

namespace {

void eraseResult(const NativeLists& lists, const OwnedShape& shape, const std::uint64_t* source, void* output)
{
    const auto cost = lists.erasedResultOwnership(shape, source);
    // Hosted values may pass between invocations and independently bound receivers.
    static std::atomic<std::uint64_t> nextIdentity{1};
    auto identity = nextIdentity.load(std::memory_order_relaxed);
    do {
        if (identity == std::numeric_limits<std::uint64_t>::max())
            throw EvaluationLimitFailure("native result identities exhausted");
    } while (!nextIdentity.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed));
    const ResultInterfaceHeader header{identity, source[0], cost.bytes, cost.elements};
    const auto& error = shape.fields[1];
    const auto errorBytes = error.shape->words * sizeof(std::uint64_t);
    const std::span storage(static_cast<std::byte*>(output), sizeof(header) + errorBytes);
    std::memcpy(storage.data(), &header, sizeof(header));
    std::memcpy(storage.subspan<sizeof(header)>().data(), source + error.offsetWords, errorBytes);
}

template<class Operation>
std::int32_t withHostedConstructionValues(void* opaque, const Operation& operation) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        operation(construction.lists);
    });
}

}

extern "C" std::int32_t pipelang_native_v19_erase_result(
    void* opaque, const OwnedShape* shape, const std::uint64_t* source, void* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        eraseResult(frame.lists, *shape, source, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_erase_hosted_result(
    void* opaque, const OwnedShape* shape, const std::uint64_t* source, void* output) noexcept
{
    return withHostedConstructionValues(opaque, [&](const NativeLists& lists) {
        eraseResult(lists, *shape, source, output);
    });
}

extern "C" std::int32_t pipelang_native_v19_validate_constructed_value(
    void* opaque, const OwnedShape* shape, const std::uint64_t* source) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        static_cast<void>(frame.lists.ownership(*shape, source));
    });
}

extern "C" std::int32_t pipelang_native_v19_validate_hosted_value(
    void* opaque, const OwnedShape* shape, const std::uint64_t* source) noexcept
{
    return withHostedConstructionValues(opaque, [&](const NativeLists& lists) {
        static_cast<void>(lists.ownership(*shape, source));
    });
}

namespace {

void requireResultInterface(std::uint64_t identity)
{
    if (identity == 0)
        throw ExecutionFailure("result interface is empty");
}

}

extern "C" std::int32_t pipelang_native_v19_require_result_interface(void* opaque, std::uint64_t identity) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame&) { requireResultInterface(identity); });
}

extern "C" std::int32_t pipelang_native_v19_hosted_require_result_interface(void* opaque, std::uint64_t identity) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction&) { requireResultInterface(identity); });
}

extern "C" std::int32_t pipelang_native_v19_owned_equal(void* opaque, const OwnedShape* shape,
                                                       const void* left, const void* right, std::int64_t* output) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        *output = frame.lists.equal(*shape, static_cast<const std::byte*>(left), static_cast<const std::byte*>(right));
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_initialization_equal(void* opaque, const OwnedShape* shape,
    const void* left, const void* right, std::int64_t* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        *output = construction.lists.equal(*shape, static_cast<const std::byte*>(left),
                                           static_cast<const std::byte*>(right));
    });
}

extern "C" std::int32_t pipelang_native_v19_validate_fixed_array(void* opaque, const OwnedShape* shape,
                                                               const void* value) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) { frame.lists.validateFixedArray(*shape, value); });
}

extern "C" std::int32_t pipelang_native_v19_hosted_validate_fixed_array(void* opaque, const OwnedShape* shape,
    const void* value) noexcept
{
    return withHostedConstructionValues(opaque, [&](const NativeLists& lists) {
        lists.validateFixedArray(*shape, value);
    });
}

extern "C" std::int32_t pipelang_native_v19_host_call(void* opaque, std::uint64_t service, const char* interfaceName,
    const char* signature, const HostArgument* arguments, std::uint64_t count, const OwnedShape* expected,
    std::uint64_t fallible, void* output) noexcept
{
    return invokeHostCall(opaque, service, interfaceName, signature, arguments, count, expected, fallible != 0, output,
                          nullptr);
}

extern "C" std::int32_t pipelang_native_v19_host_call_with_reusable_bytes(void* opaque, std::uint64_t service,
    const char* interfaceName, const char* signature, const HostArgument* arguments, std::uint64_t count,
    const OwnedShape* expected, std::uint64_t fallible, void* output, const void* byteSlot) noexcept
{
    return invokeHostCall(opaque, service, interfaceName, signature, arguments, count, expected, fallible != 0, output,
                          byteSlot);
}

extern "C" std::int32_t pipelang_native_v19_hosted_initialization_call(void* opaque, std::uint64_t service,
    const char* interfaceName, const char* signature, const HostArgument* arguments, std::uint64_t count,
    const OwnedShape* expected, std::uint64_t fallible, void* output) noexcept
{
    return withHostedConstruction(opaque, [&](NativeHostedConstruction& construction) {
        const auto call = decodeHostCall(construction.services, construction.lists, service, interfaceName,
                                         signature, {arguments, count}, expected);
        auto& scope = construction.storage.contextScope();
        const auto result = scope.invokeSynchronousHost([&] {
            return call.service->invoke(call.signature, call.arguments);
        });
        const auto retain = [&](std::string_view value) { return construction.values.retainText(value); };
        encodeHostResult(construction.values, construction.lists, result, expected, fallible != 0, output, retain, retain);
    });
}

extern "C" std::int32_t pipelang_native_v19_prepare_host_call(void* opaque, std::uint64_t service,
    const char* interfaceName, const char* signature, const HostArgument* arguments, std::uint64_t count,
    const OwnedShape* expected) noexcept
{
    return invokeOperation(opaque, [&](RuntimeFrame& frame) {
        if (frame.pendingHost)
            throw ExecutionFailure("native host call is already pending");
        frame.pendingHost.emplace(decodeHostCall(frame.services, frame.lists, service, interfaceName, signature, {arguments, count}, expected));
    });
}
