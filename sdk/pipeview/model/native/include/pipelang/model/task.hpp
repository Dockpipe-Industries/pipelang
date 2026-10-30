// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pipelang::model {

class CancellationToken {
public:
    bool is_cancelled() const {
        for (const auto& flag : flags_) {
            if (flag->load(std::memory_order_acquire))
                return true;
        }
        return false;
    }
    static CancellationToken combine(CancellationToken left, const CancellationToken& right) {
        for (const auto& flag : right.flags_) {
            if (std::find(left.flags_.begin(), left.flags_.end(), flag) == left.flags_.end())
                left.flags_.push_back(flag);
        }
        if (left.flags_.size() > 64)
            throw ExecutionFailure("cancellation ancestry limit exceeded");
        return left;
    }
private:
    friend class CancellationSource;
    std::vector<std::shared_ptr<std::atomic<bool>>> flags_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const {
        CancellationToken result;
        result.flags_.push_back(flag_);
        return result;
    }
    void cancel() const { flag_->store(true, std::memory_order_release); }
private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

struct TaskOptions {
    std::size_t capacity = 4;
    static constexpr std::size_t maximumCapacity = 32;
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

// One owner executor, one worker, bounded admitted coroutines. Host jobs and
// completions have FIFO order. Worker callbacks never access model state. The
// owner must pump poll(); no continuation is run by the worker or another owner.
class TaskScope {
public:
    explicit TaskScope(TaskOptions options = {})
        : owner_(std::this_thread::get_id()), capacity_(options.capacity) {
        if (capacity_ == 0 || capacity_ > TaskOptions::maximumCapacity)
            throw ExecutionFailure("task capacity must be between 1 and 32");
        worker_ = std::thread([this] { work(); });
    }
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    ~TaskScope() {
        // A model must be destroyed on its owner. Failing closed here avoids a
        // destructor racing a continuation or destroying an active frame.
        if (owner_ != std::this_thread::get_id())
            std::terminate();
        for (auto& entry : entries_)
            entry.cancel();
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        worker_.join();
        // Preserve authoritative host outcomes already produced, including a
        // successful external commit whose completion has not yet been polled.
        // The generated owner destroys this scope before its state and services.
        // Closing awaits complete immediately as Cancelled, so these finite
        // source continuations cannot create more host work during teardown.
        std::deque<std::function<void()>> finalCompletions;
        {
            std::lock_guard lock(mutex_);
            finalCompletions.swap(completed_);
        }
        for (auto& completion : finalCompletions)
            completion();
        abandoningFrames_ = true;
        for (auto& entry : entries_) {
            entry.abandon();
            entry.handle.destroy();
        }
    }
    bool abandoningFrames() const noexcept { return abandoningFrames_; }
    void requireOwner() const {
        if (owner_ != std::this_thread::get_id())
            throw ExecutionFailure("model accessed from a different owner thread");
    }
    std::size_t pending() const {
        requireOwner();
        return entries_.size();
    }
    template<class T, class Factory>
    Task<T> start(Factory factory) {
        requireOwner();
        reap();
        if (entries_.size() >= capacity_) {
            auto state = std::make_shared<TaskState<T>>();
            state->result = T::Failure("task capacity exceeded");
            return Task<T>(std::move(state));
        }
        CancellationSource cancellation;
        auto coroutine = factory(cancellation.token());
        auto handle = coroutine.handle;
        auto state = handle.promise().state;
        state->cancellation = cancellation;
        Entry entry;
        entry.handle = handle;
        entry.cancel = [state] { state->cancellation.cancel(); };
        entry.abandon = [state] {
            if (!state->result)
                state->result = T::Cancelled("task owner destroyed");
        };
        entries_.push_back(std::move(entry));
        coroutine.release();
        handle.resume();
        reap();
        return Task<T>(std::move(state));
    }
    std::size_t poll() {
        requireOwner();
        // Only the queue present at entry is delivered. Jobs submitted by these
        // continuations are delivered on a later poll, avoiding an unbounded pump.
        std::deque<std::function<void()>> ready;
        {
            std::lock_guard lock(mutex_);
            ready.swap(completed_);
        }
        const auto count = ready.size();
        for (auto& completion : ready)
            completion();
        reap();
        return count;
    }
    template<class T>
    class HostAwaitable {
    public:
        HostAwaitable(TaskScope& scope, std::function<T()> function, CancellationToken cancellation)
            : scope_(scope), function_(std::move(function)), cancellation_(std::move(cancellation)) {}
        bool await_ready() {
            scope_.requireOwner();
            if (!scope_.stopping_)
                return false;
            *result_ = T::Cancelled("task owner is closing");
            return true;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            scope_.requireOwner();
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
                }, [continuation] { continuation.resume(); });
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
        std::shared_ptr<std::optional<T>> result_ = std::make_shared<std::optional<T>>();
        std::shared_ptr<bool> suspended_ = std::make_shared<bool>(false);
    };
    template<class T>
    HostAwaitable<T> host(std::function<T()> function, CancellationToken cancellation) {
        return HostAwaitable<T>(*this, std::move(function), std::move(cancellation));
    }
private:
    struct Entry {
        std::coroutine_handle<> handle;
        std::function<void()> cancel;
        std::function<void()> abandon;
    };
    struct Job {
        std::function<void()> execute;
        std::function<void()> complete;
    };
    void reap() {
        auto entry = entries_.begin();
        while (entry != entries_.end()) {
            if (entry->handle.done()) {
                entry->handle.destroy();
                entry = entries_.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    void submit(std::function<void()> execute, std::function<void()> complete) {
        std::lock_guard lock(mutex_);
        if (stopping_ || jobs_.size() >= capacity_)
            throw ExecutionFailure("host queue unavailable");
        jobs_.push_back({std::move(execute), std::move(complete)});
        wake_.notify_one();
    }
    void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty())
                    return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job.execute();
            {
                std::lock_guard lock(mutex_);
                completed_.push_back(std::move(job.complete));
            }
        }
    }
    std::thread::id owner_;
    std::size_t capacity_;
    std::thread worker_;
    std::vector<Entry> entries_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<std::function<void()>> completed_;
    bool stopping_ = false;
    bool abandoningFrames_ = false;
};

}
