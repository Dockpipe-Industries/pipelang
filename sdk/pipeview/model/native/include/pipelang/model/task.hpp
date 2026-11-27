// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"
#include "cancellation.hpp"
#include "context.hpp"
#include "limits.hpp"
#include "task_group.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pipelang::model {

namespace detail {
struct NativeTaskPollAccess;
}

struct TaskOptions {
    std::size_t capacity = 4;
    std::size_t initializationSteps = defaultEvaluationSteps;
    std::size_t evaluationStepsPerSegment = defaultEvaluationSteps;
    static constexpr std::size_t maximumCapacity = 32;
    static constexpr std::size_t maximumEvaluationStepsPerSegment = pipelang::model::maximumEvaluationStepsPerSegment;
    static constexpr std::size_t maximumInitializationSteps = pipelang::model::maximumInitializationSteps;
};

template<class T>
struct TaskState {
    std::optional<T> result;
    CancellationSource cancellation;
    std::thread::id owner = std::this_thread::get_id();
};

template<class T>
class Task {
public:
    explicit Task(std::shared_ptr<TaskState<T>> state) : state_(std::move(state)) {}
    bool completed() const {
        requireOwner();
        return state_->result.has_value();
    }
    const T& result() const {
        requireOwner();
        if (!state_->result)
            throw ExecutionFailure("task is pending");
        return *state_->result;
    }
    // Cancellation request is thread-safe; result observation is owner-only.
    void cancel() const { state_->cancellation.cancel(); }
private:
    void requireOwner() const {
        if (state_->owner != std::this_thread::get_id())
            throw ExecutionFailure("task observed from a different owner thread");
    }
    std::shared_ptr<TaskState<T>> state_;
};

template<class T>
struct Coroutine {
    struct promise_type {
        std::shared_ptr<TaskState<T>> state = std::make_shared<TaskState<T>>();
        std::optional<T> pendingResult;
        // Native task observation keeps its Result contract. A source parent
        // awaiting this frame must retain the terminal identity of exhaustion,
        // rather than treating it as an authored recoverable Failure value.
        std::exception_ptr terminalFailure;
        Coroutine get_return_object() {
            return Coroutine(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
                static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                    "owned coroutine results require nonthrowing publication");
                auto& promise = handle.promise();
                if (!promise.pendingResult)
                    std::terminate();
                // A cleanup fault can be caught by source recovery after an
                // attempted co_return. Publish only once final cleanup finishes.
                promise.state->result = std::move(promise.pendingResult);
                return promise.continuation;
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        std::coroutine_handle<> continuation = std::noop_coroutine();
        void return_value(T value) { pendingResult = std::move(value); }
        void unhandled_exception() {
            try {
                throw;
            } catch (const OperationCancelled& error) {
                pendingResult = T::Cancelled(safeErrorMessage(error));
            } catch (const EvaluationLimitFailure& error) {
                terminalFailure = std::current_exception();
                pendingResult = T::Failure(safeErrorMessage(error));
            } catch (const std::exception& error) {
                pendingResult = T::Failure(safeErrorMessage(error));
            } catch (...) {
                pendingResult = T::Failure("unexpected task failure");
            }
        }
    };
    using Handle = std::coroutine_handle<promise_type>;
    explicit Coroutine(Handle value) : handle(value) {}
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;
    Coroutine(Coroutine&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    ~Coroutine() {
        if (handle)
            handle.destroy();
    }
    class Awaiter {
    public:
        explicit Awaiter(Handle child) : child_(child) {}
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&& other) noexcept : child_(std::exchange(other.child_, {})) {}
        ~Awaiter() {
            if (child_)
                child_.destroy();
        }
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            child_.promise().continuation = parent;
            return child_;
        }
        T await_resume() {
            if (child_.promise().terminalFailure)
                std::rethrow_exception(child_.promise().terminalFailure);
            if (!child_.promise().state->result)
                throw ExecutionFailure("child coroutine completed without a result");
            // The awaiter retains exclusive frame ownership through result
            // extraction, including an exceptional result move.
            T result = std::move(*child_.promise().state->result);
            child_.destroy();
            child_ = {};
            return result;
        }
    private:
        Handle child_;
    };
    Awaiter operator co_await() && noexcept {
        return Awaiter(std::exchange(handle, {}));
    }
    void release() noexcept { handle = {}; }
    Handle handle;
};

// One owner executor, one lazily started worker, bounded admitted coroutines.
// Host jobs and completions have FIFO order. Worker callbacks never access model
// state. The owner must pump poll(); no continuation runs on the worker or another owner.
class TaskScope {
    // One admitted root owns this state throughout symmetric child transfers.
    // Only owner-thread continuations touch it. Await descriptors retain their
    // entry explicitly; no cleanup flag is left active on the owner executor.
    struct Execution {
        std::size_t waits = 0;
        unsigned cleanupDepth = 0;
        std::shared_ptr<const unsigned char> cleanupPhase;
        bool settled = false;
        void (*pollCheck)() = nullptr;
    };

    struct PollState {
        std::function<std::optional<bool>()> check;
        CancellationToken cancellation;
        std::optional<bool> result;
        std::exception_ptr failure;
        std::coroutine_handle<> continuation;
        bool cancelled = false;
        bool active = false;
        std::shared_ptr<TaskGroup> group;
        InvocationPermit permit;
        std::shared_ptr<Execution> execution;
        std::shared_ptr<const unsigned char> cleanupPhase;
    };

public:
    class CleanupRegion {
    public:
        CleanupRegion(const CleanupRegion&) = delete;
        CleanupRegion& operator=(const CleanupRegion&) = delete;
        ~CleanupRegion() {
            cancellation_ = std::move(previousCancellation_);
            if (--execution_->cleanupDepth == 0)
                execution_->cleanupPhase.reset();
        }

    private:
        friend class TaskScope;
        CleanupRegion(std::shared_ptr<Execution> execution, CancellationToken& cancellation)
            : execution_(std::move(execution)), cancellation_(cancellation),
              previousCancellation_(cancellation) {
            // Nested cleanup and its source callees retain an explicitly chosen
            // cleanup token. Only the first region shields normal cancellation.
            if (execution_->cleanupDepth == 0) {
                // A retained native await descriptor cannot carry this authority
                // into a later phase of the same task. Retention prevents identity
                // reuse without a wrapping generation counter.
                execution_->cleanupPhase = std::make_shared<const unsigned char>(0);
                cancellation_ = {};
            }
            ++execution_->cleanupDepth;
        }

        std::shared_ptr<Execution> execution_;
        CancellationToken& cancellation_;
        CancellationToken previousCancellation_;
    };

    [[nodiscard]] CleanupRegion enterCleanup(CancellationToken& cancellation);

    // Only the external callback is guarded. Source helpers remain in their
    // existing outer transaction; this guard neither commits nor rolls back
    // model state or external effects. The scope must outlive the guard.
    class SynchronousHostCallGuard {
    public:
        SynchronousHostCallGuard(const SynchronousHostCallGuard&) = delete;
        SynchronousHostCallGuard& operator=(const SynchronousHostCallGuard&) = delete;

        SynchronousHostCallGuard(SynchronousHostCallGuard&& other) noexcept
            : scope_(std::exchange(other.scope_, nullptr)) {}

        SynchronousHostCallGuard& operator=(SynchronousHostCallGuard&& other) noexcept {
            if (this != &other) {
                release();
                scope_ = std::exchange(other.scope_, nullptr);
            }
            return *this;
        }

        ~SynchronousHostCallGuard() { release(); }

    private:
        friend class TaskScope;
        explicit SynchronousHostCallGuard(TaskScope& scope) : scope_(&scope) {
            scope.requireOwner();
            scope.synchronousHostCallActive_ = true;
        }

        void release() noexcept {
            if (!scope_)
                return;
            // Moving the guard transfers ownership, never its thread affinity.
            if (scope_->owner_ != std::this_thread::get_id())
                std::terminate();
            scope_->synchronousHostCallActive_ = false;
            scope_ = nullptr;
        }

        TaskScope* scope_;
    };

    [[nodiscard]] SynchronousHostCallGuard enterSynchronousHostCall();

    // A synchronous host boundary stays on the owner thread. It owns no
    // worker job or commit point, and preserves cancellation as a distinct fault.
    template<class Callback>
    auto invokeSynchronousHost(Callback&& callback) {
        auto guard = enterSynchronousHostCall();
        if (cleanupActive())
            currentPermit().requireCurrent();
        try {
            auto result = std::invoke(std::forward<Callback>(callback));
            validate(result);
            return result;
        } catch (const OperationCancelled&) {
            throw;
        } catch (const ExecutionFailure&) {
            throw;
        } catch (const std::exception& error) {
            throw ExecutionFailure(safeErrorMessage(error));
        } catch (...) {
            throw ExecutionFailure("unexpected synchronous host service failure");
        }
    }

    static constexpr std::size_t maximumPollingWaits = 32;
    std::shared_ptr<ContextOwner> createContextOwner();

    explicit TaskScope(TaskOptions options = {});
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    ~TaskScope();
    // Idempotent owner-only cancellation/drain, reused by composed owners before
    // their resource graph is destroyed. Host callbacks must honor cancellation
    // or their own bounded deadlines; arbitrary native work cannot be force-killed.
    void shutdown();
    bool abandoningFrames() const noexcept;
    void requireOwner() const;
    std::size_t evaluationStepsPerSegment() const;
    std::size_t pending() const;
    std::shared_ptr<TaskGroup> createGroup(std::shared_ptr<TaskGroup> parent = {});
    void closeGroup(const std::shared_ptr<TaskGroup>& group);
    std::size_t pending(const std::shared_ptr<TaskGroup>& group) const;
    // Drain one lifetime subtree without stopping independent groups. Cancellation
    // is cooperative; host work must supply its own bounded I/O deadline.
    void drainGroup(const std::shared_ptr<TaskGroup>& group);
    template<class T, class Factory>
    Result<Task<T>> tryStart(const std::shared_ptr<TaskGroup>& group, Factory factory) {
        using Callable = std::remove_reference_t<decltype(factory)>;
        using CreatedCoroutine = std::remove_cvref_t<decltype(factory(
            CancellationToken::combine(std::declval<const CancellationSource&>().token(),
                                       std::declval<const InvocationPermit&>().cancellation())))>;
        const BorrowedFactory<CreatedCoroutine> invocation{
            // Restore the original callable cv qualification in the thunk.
            const_cast<void*>(static_cast<const volatile void*>(std::addressof(factory))),
            [](void* context, const CancellationSource& cancellation,
               const InvocationPermit& permit) -> CreatedCoroutine {
                auto& callable = *static_cast<Callable*>(context);
                return callable(CancellationToken::combine(cancellation.token(), permit.cancellation()));
            }
        };
        return tryStartCore<T>(group, invocation);
    }
    template<class T, class Factory>
    Task<T> start(Factory factory) {
        const auto admitted = tryStart<T>(activeGroup_, std::move(factory));
        return taskFromAdmission(admitted);
    }
    std::size_t poll();
    class PollingAwaitable {
    public:
        PollingAwaitable(TaskScope& scope, std::function<std::optional<bool>()> check,
                         CancellationToken cancellation)
            : scope_(scope), state_(std::make_shared<PollState>()) {
            state_->check = std::move(check);
            state_->permit = currentPermit();
            state_->cancellation = CancellationToken::combine(std::move(cancellation), state_->permit.cancellation());
            state_->group = scope_.activeGroup_;
            state_->execution = scope_.activeExecution_;
            state_->cleanupPhase = scope_.activeCleanupPhase();
        }

        PollingAwaitable(const PollingAwaitable&) = delete;
        PollingAwaitable& operator=(const PollingAwaitable&) = delete;
        PollingAwaitable(PollingAwaitable&&) noexcept = default;

        ~PollingAwaitable() {
            if (state_) {
                // Frame destruction invalidates a registration even if its
                // slot remains in an already captured poll snapshot.
                if (state_->active && state_->execution)
                    --state_->execution->waits;
                state_->active = false;
                state_->continuation = {};
                state_->check = {};
            }
        }

        bool await_ready() {
            scope_.requireAwaitOwner(state_->execution, state_->cleanupPhase);
            return scope_.evaluatePoll(*state_, scope_.closingWait(state_->group, state_->execution, state_->cleanupPhase));
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            scope_.requireAwaitOwner(state_->execution, state_->cleanupPhase);
            if (!state_->execution && scope_.activeExecution_) {
                state_->execution = scope_.activeExecution_;
                state_->group = scope_.activeGroup_;
                state_->cleanupPhase = scope_.activeCleanupPhase();
            }
            if (scope_.closingWait(state_->group, state_->execution, state_->cleanupPhase)) {
                scope_.evaluatePoll(*state_, true);
                return false;
            }
            for (auto& slot : scope_.polling_) {
                if (!slot || !slot->active) {
                    state_->continuation = continuation;
                    state_->active = true;
                    if (state_->execution)
                        ++state_->execution->waits;
                    slot = state_;
                    *suspended_ = true;
                    return true;
                }
            }
            throw ExecutionFailure("polling wait capacity exceeded");
        }

        bool await_resume() {
            if (state_->failure)
                std::rethrow_exception(state_->failure);
            if (state_->cancelled)
                throw OperationCancelled("operation cancelled");
            if (!state_->result)
                throw ExecutionFailure("polling wait completed without a result");
            return *state_->result;
        }

        std::shared_ptr<const bool> suspensionState() const {
            return suspended_;
        }

    private:
        TaskScope& scope_;
        std::shared_ptr<PollState> state_;
        std::shared_ptr<bool> suspended_ = std::make_shared<bool>(false);
    };

    PollingAwaitable pollUntil(std::function<std::optional<bool>()> check,
                              CancellationToken cancellation = {});
    template<class T>
    class HostAwaitable {
    public:
        HostAwaitable(TaskScope& scope, std::function<T()> function, CancellationToken cancellation)
            : scope_(scope), function_(std::move(function)), cancellation_(std::move(cancellation)),
              group_(scope.activeGroup_), permit_(currentPermit()),
              execution_(scope.activeExecution_), cleanupPhase_(scope.activeCleanupPhase()) {
            cancellation_ = CancellationToken::combine(std::move(cancellation_), permit_.cancellation());
        }
        bool await_ready() {
            scope_.requireAwaitOwner(execution_, cleanupPhase_);
            if (!scope_.closingWait(group_, execution_, cleanupPhase_))
                return false;
            *result_ = T::Cancelled("task owner is closing");
            return true;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            scope_.requireAwaitOwner(execution_, cleanupPhase_);
            if (!execution_ && scope_.activeExecution_) {
                execution_ = scope_.activeExecution_;
                group_ = scope_.activeGroup_;
                cleanupPhase_ = scope_.activeCleanupPhase();
            }
            try {
                auto result = result_;
                auto cancellation = cancellation_;
                auto function = std::move(function_);
                scope_.submit([result, cancellation, function = std::move(function)]() mutable {
                    if (cancellation.is_cancelled()) {
                        *result = T::Cancelled("operation cancelled");
                        return;
                    }
                    try {
                        *result = function();
                        // Host success is authoritative: a completed external write
                        // must not be relabelled cancelled after it has committed.
                        validate(**result);
                    } catch (const std::exception& error) {
                        *result = T::Failure(safeErrorMessage(error));
                    } catch (...) {
                        *result = T::Failure("unexpected host service failure");
                    }
                }, [scope = &scope_, continuation, group = group_, permit = permit_, execution = execution_] {
                    if (execution) {
                        --execution->waits;
                        if (execution->settled)
                            return;
                    }
                    scope->resume(continuation, group, permit, execution);
                }, execution_, cleanupPhase_);
                if (execution_)
                    ++execution_->waits;
                *suspended_ = true;
                return true;
            } catch (const std::exception& error) {
                *result_ = T::Failure(safeErrorMessage(error));
            } catch (...) {
                *result_ = T::Failure("unexpected host submission failure");
            }
            // submit transfers queue ownership only on success. A refusal leaves
            // no continuation queued; returning false delivers the typed failure
            // immediately through await_resume so authored cleanup still runs.
            return false;
        }
        T await_resume() { return std::move(**result_); }
        std::shared_ptr<const bool> suspensionState() const { return suspended_; }
    private:
        TaskScope& scope_;
        std::function<T()> function_;
        CancellationToken cancellation_;
        std::shared_ptr<TaskGroup> group_;
        InvocationPermit permit_;
        std::shared_ptr<Execution> execution_;
        std::shared_ptr<const unsigned char> cleanupPhase_;
        std::shared_ptr<std::optional<T>> result_ = std::make_shared<std::optional<T>>();
        std::shared_ptr<bool> suspended_ = std::make_shared<bool>(false);
    };
    template<class T>
    HostAwaitable<T> host(std::function<T()> function, CancellationToken cancellation) {
        return HostAwaitable<T>(*this, std::move(function), std::move(cancellation));
    }
private:
    friend struct detail::NativeTaskPollAccess;

    // A source invocation can validate the owner environment before a poll
    // consumes a queued completion or evaluates a wait.
    void setCurrentExecutionPollCheck(void (*check)()) {
        requireOwner();
        if (!activeExecution_ || activeExecution_->settled || !check)
            throw ExecutionFailure("poll validation requires an active task execution");
        if (activeExecution_->pollCheck && activeExecution_->pollCheck != check)
            throw ExecutionFailure("task execution already has a different poll check");
        activeExecution_->pollCheck = check;
    }

    // The factory is borrowed only for synchronous admission. Keeping its type
    // out of the owning algorithm shares entry callbacks across factory closures.
    template<class CreatedCoroutine>
    struct BorrowedFactory {
        void* context;
        CreatedCoroutine (*invoke)(void*, const CancellationSource&, const InvocationPermit&);
    };

    template<class T, class CreatedCoroutine>
    Result<Task<T>> tryStartCore(const std::shared_ptr<TaskGroup>& group,
                                BorrowedFactory<CreatedCoroutine> factory) {
        requireOwner();
        if (group)
            requireGroup(group);
        reap();
        if (closing_ || groupClosing(group))
            return Result<Task<T>>::Failure("task group is closing");
        if (entries_.size() >= capacity_)
            return Result<Task<T>>::Failure("task capacity exceeded");
        const auto permit = currentPermit();
        if (!permit.is_current())
            return Result<Task<T>>::Cancelled("context.stale");
        CancellationSource cancellation;
        auto coroutine = factory.invoke(factory.context, cancellation, permit);
        auto handle = coroutine.handle;
        auto state = handle.promise().state;
        state->cancellation = cancellation;
        Entry entry;
        entry.execution = std::make_shared<Execution>();
        entry.handle = handle;
        entry.group = group;
        entry.permit = permit;
        entry.cancel = [state] { state->cancellation.cancel(); };
        entry.abandon = [state] {
            if (!state->result)
                state->result = T::Cancelled("task owner destroyed");
        };
        const auto execution = entry.execution;
        entries_.push_back(std::move(entry));
        coroutine.release();
        resume(handle, group, permit, execution);
        reap();
        return Result<Task<T>>::Success(Task<T>(std::move(state)));
    }

    template<class T>
    static Task<T> taskFromAdmission(const Result<Task<T>>& admitted) {
        if (!admitted.is_success()) {
            auto state = std::make_shared<TaskState<T>>();
            if (admitted.is_cancelled())
                state->result = T::Cancelled(admitted.error());
            else
                state->result = T::Failure(admitted.error());
            return Task<T>(std::move(state));
        }
        return admitted.value();
    }

    bool evaluatePoll(PollState& state, bool closing);

    void completePoll(const std::shared_ptr<PollState>& state);

    void requireGroup(const std::shared_ptr<TaskGroup>& group) const;
    static bool belongsTo(std::shared_ptr<TaskGroup> candidate, const std::shared_ptr<TaskGroup>& ancestor);
    static bool groupClosing(std::shared_ptr<TaskGroup> group);
    void requireAwaitOwner(const std::shared_ptr<Execution>& captured,
        const std::shared_ptr<const unsigned char>& cleanupPhase) const;
    std::shared_ptr<const unsigned char> activeCleanupPhase() const noexcept;
    bool cleanupActive() const noexcept;
    bool closingWait(const std::shared_ptr<TaskGroup>& group,
        const std::shared_ptr<Execution>& execution,
        const std::shared_ptr<const unsigned char>& cleanupPhase) const;
    void resume(std::coroutine_handle<> continuation, const std::shared_ptr<TaskGroup>& group,
        InvocationPermit permit, const std::shared_ptr<Execution>& execution);

    struct Entry {
        std::coroutine_handle<> handle;
        std::function<void()> cancel;
        std::function<void()> abandon;
        std::shared_ptr<TaskGroup> group;
        InvocationPermit permit;
        std::shared_ptr<Execution> execution;
    };
    struct Job {
        std::function<void()> execute;
        std::function<void()> complete;
    };
    void reap();
    // Native clients may deliberately suspend without registering a wakeup.
    // Generated source cannot: its leaf awaits are host/polling registrations.
    // Preserve the SDK's nonthrowing abandonment fallback for those dormant frames.
    void abandonDormant(const std::shared_ptr<TaskGroup>& group = {});
    void submit(std::function<void()> execute, std::function<void()> complete,
        const std::shared_ptr<Execution>& execution,
        const std::shared_ptr<const unsigned char>& cleanupPhase);
    void work();
    inline static thread_local TaskScope* resumingScope_ = nullptr;
    std::thread::id owner_;
    InvocationPermit lifetimePermit_;
    std::unique_ptr<ContextOwner> contextRoot_;
    std::size_t capacity_;
    const std::size_t evaluationStepsPerSegment_;
    std::thread worker_;
    std::vector<Entry> entries_;
    std::array<std::shared_ptr<PollState>, maximumPollingWaits> polling_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<std::function<void()>> completed_;
    bool closing_ = false;
    bool stopping_ = false;
    bool abandoningFrames_ = false;
    bool synchronousHostCallActive_ = false;
    bool shutdownComplete_ = false;
    std::shared_ptr<const void> groupIdentity_;
    std::shared_ptr<TaskGroup> activeGroup_;
    std::shared_ptr<Execution> activeExecution_;
    std::size_t resumeDepth_ = 0;
};

}

#if !defined(PIPELANG_MODEL_LINKED_RUNTIME) || !PIPELANG_MODEL_LINKED_RUNTIME
#define PIPELANG_MODEL_RUNTIME_INLINE inline
#include "detail/task_scope_impl.hpp"
#undef PIPELANG_MODEL_RUNTIME_INLINE
#endif
