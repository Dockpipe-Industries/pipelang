// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef PIPELANG_MODEL_RUNTIME_INLINE
#error "Include task_scope_impl.hpp through task.hpp or its owning runtime translation unit"
#endif

namespace pipelang::model {

PIPELANG_MODEL_RUNTIME_INLINE TaskScope::CleanupRegion TaskScope::enterCleanup(CancellationToken& cancellation) {
    requireOwner();
    if (!activeExecution_ || activeExecution_->settled)
        throw ExecutionFailure("cleanup requires an admitted task continuation");
    return CleanupRegion(activeExecution_, cancellation);
}

PIPELANG_MODEL_RUNTIME_INLINE TaskScope::SynchronousHostCallGuard TaskScope::enterSynchronousHostCall() {
    return SynchronousHostCallGuard(*this);
}

PIPELANG_MODEL_RUNTIME_INLINE std::shared_ptr<ContextOwner> TaskScope::createContextOwner() {
    requireOwner();
    if (closing_)
        throw ExecutionFailure("task owner is closing");
    const auto permit = currentPermit();
    permit.requireCurrent();
    lifetimePermit_.requireCurrent();
    if (!contextRoot_) {
        // An unbound model may first create a resource under an operation.
        // Adopt that operation's logical root, but never its cancellation
        // as the model lifetime: later sibling operations must stay usable.
        const auto rootIdentity = lifetimePermit_.is_bound()
            ? lifetimePermit_.rootIdentity() : permit.rootIdentity();
        contextRoot_ = std::unique_ptr<ContextOwner>(new ContextOwner(
            ContextOwner::LifetimeRoot{}, lifetimePermit_, rootIdentity));
    }
    const auto parent = InvocationPermit::combine(contextRoot_->capture().permit(), permit);
    return std::make_shared<ContextOwner>(parent);
}

PIPELANG_MODEL_RUNTIME_INLINE TaskScope::TaskScope(TaskOptions options)
    : owner_(std::this_thread::get_id()), lifetimePermit_(currentPermit()), capacity_(options.capacity),
      evaluationStepsPerSegment_(options.evaluationStepsPerSegment) {
    if (capacity_ == 0 || capacity_ > TaskOptions::maximumCapacity)
        throw ExecutionFailure("task capacity must be between 1 and 32");
    requireInitializationSteps(options.initializationSteps);
    requireEvaluationStepsPerSegment(evaluationStepsPerSegment_);
}

PIPELANG_MODEL_RUNTIME_INLINE TaskScope::~TaskScope() {
    // A model must be destroyed on its owner. Failing closed here avoids a
    // destructor racing a continuation or destroying an active frame.
    if (owner_ != std::this_thread::get_id() || synchronousHostCallActive_)
        std::terminate();
    shutdown();
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::shutdown() {
    requireOwner();
    if (resumeDepth_ != 0)
        throw ExecutionFailure("task shutdown during an active continuation");
    if (shutdownComplete_)
        return;
    closing_ = true;
    for (auto& entry : entries_)
        entry.cancel();
    // Keep the worker, services and resource lifetime root alive until all
    // registered continuations have completed their admitted cleanup. Normal
    // closing awaits cancel; an entry-owned cleanup await may still submit.
    while (!entries_.empty()) {
        poll();
        abandonDormant();
        if (!entries_.empty()) {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(1),
                [this] { return !completed_.empty(); });
        }
    }
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable())
        worker_.join();
    // Native clients may register standalone awaiters with noop continuations.
    // Preserve their authoritative completions and cancel their polling waits.
    std::deque<std::function<void()>> finalCompletions;
    {
        std::lock_guard lock(mutex_);
        finalCompletions.swap(completed_);
    }
    for (auto& completion : finalCompletions)
        completion();
    const auto closingWaits = polling_;
    for (const auto& state : closingWaits) {
        if (state && state->active) {
            evaluatePoll(*state, true);
            completePoll(state);
        }
    }
    contextRoot_.reset();
    shutdownComplete_ = true;
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::abandoningFrames() const noexcept { return abandoningFrames_; }

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::requireOwner() const {
    if (owner_ != std::this_thread::get_id())
        throw ExecutionFailure("model accessed from a different owner thread");
    if (synchronousHostCallActive_)
        throw ExecutionFailure("model reentry during a synchronous host callback");
}

PIPELANG_MODEL_RUNTIME_INLINE std::size_t TaskScope::evaluationStepsPerSegment() const {
    requireOwner();
    return evaluationStepsPerSegment_;
}

PIPELANG_MODEL_RUNTIME_INLINE std::size_t TaskScope::pending() const {
    requireOwner();
    return entries_.size();
}

PIPELANG_MODEL_RUNTIME_INLINE std::shared_ptr<TaskGroup> TaskScope::createGroup(std::shared_ptr<TaskGroup> parent) {
    requireOwner();
    if (closing_)
        throw ExecutionFailure("task owner is closing");
    if (parent)
        requireGroup(parent);
    std::size_t depth = 0;
    for (auto ancestor = parent; ancestor; ancestor = ancestor->parent_) {
        if (++depth >= 64)
            throw ExecutionFailure("task group ancestry limit exceeded");
    }
    if (groupClosing(parent))
        throw ExecutionFailure("task group is closing");
    if (!groupIdentity_)
        groupIdentity_ = std::make_shared<unsigned char>(0);
    return std::shared_ptr<TaskGroup>(new TaskGroup(groupIdentity_, std::move(parent)));
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::closeGroup(const std::shared_ptr<TaskGroup>& group) {
    requireOwner();
    requireGroup(group);
    group->closing_ = true;
    for (auto& entry : entries_) {
        if (belongsTo(entry.group, group))
            entry.cancel();
    }
}

PIPELANG_MODEL_RUNTIME_INLINE std::size_t TaskScope::pending(const std::shared_ptr<TaskGroup>& group) const {
    requireOwner();
    requireGroup(group);
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return belongsTo(entry.group, group);
    }));
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::drainGroup(const std::shared_ptr<TaskGroup>& group) {
    requireOwner();
    if (resumeDepth_ != 0)
        throw ExecutionFailure("task drain during an active continuation");
    closeGroup(group);
    while (pending(group) != 0) {
        poll();
        abandonDormant(group);
        if (pending(group) == 0)
            break;
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(1), [this] { return !completed_.empty(); });
    }
}

PIPELANG_MODEL_RUNTIME_INLINE std::size_t TaskScope::poll() {
    requireOwner();
    // A refused poll must leave all completed work and registered waits intact.
    // Draining a closing execution must still deliver its cancellation and cleanup.
    if (!closing_) {
        for (const auto& entry : entries_) {
            if (!entry.handle.done() && entry.execution->pollCheck && !groupClosing(entry.group))
                entry.execution->pollCheck();
        }
    }
    // Capture registrations before host continuations run. A continuation
    // may register another wait, which belongs to the next owner poll.
    const auto pollingSnapshot = polling_;
    // Only the queue present at entry is delivered. Jobs submitted by these
    // continuations are delivered on a later poll, avoiding an unbounded pump.
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(completed_);
    }
    auto count = ready.size();
    for (auto& completion : ready)
        completion();
    for (const auto& state : pollingSnapshot) {
        if (state && state->active && evaluatePoll(*state, closingWait(state->group, state->execution, state->cleanupPhase))) {
            completePoll(state);
            ++count;
        }
    }
    reap();
    return count;
}

PIPELANG_MODEL_RUNTIME_INLINE TaskScope::PollingAwaitable TaskScope::pollUntil(std::function<std::optional<bool>()> check,
                          CancellationToken cancellation) {
    requireOwner();
    return PollingAwaitable(*this, std::move(check), std::move(cancellation));
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::evaluatePoll(PollState& state, bool closing) {
    try {
        if (closing || state.cancellation.is_cancelled()) {
            state.cancelled = true;
            return true;
        }
        InvocationContextScope invocation(state.permit);
        state.result = state.check();
        return state.result.has_value();
    } catch (...) {
        state.failure = std::current_exception();
        return true;
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::completePoll(const std::shared_ptr<PollState>& state) {
    // Unregister before resuming: the continuation may destroy this awaiter
    // and immediately reuse a slot for a different operation.
    state->active = false;
    if (state->execution)
        --state->execution->waits;
    for (auto& slot : polling_) {
        if (slot == state)
            slot.reset();
    }
    auto continuation = std::exchange(state->continuation, {});
    state->check = {};
    if (continuation && (!state->execution || !state->execution->settled))
        resume(continuation, state->group, state->permit, state->execution);
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::requireGroup(const std::shared_ptr<TaskGroup>& group) const {
    if (!group || group->identity_ != groupIdentity_)
        throw ExecutionFailure("task group belongs to a different owner");
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::belongsTo(std::shared_ptr<TaskGroup> candidate, const std::shared_ptr<TaskGroup>& ancestor) {
    while (candidate) {
        if (candidate == ancestor)
            return true;
        candidate = candidate->parent_;
    }
    return false;
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::groupClosing(std::shared_ptr<TaskGroup> group) {
    while (group) {
        if (group->closing_)
            return true;
        group = group->parent_;
    }
    return false;
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::requireAwaitOwner(const std::shared_ptr<Execution>& captured,
    const std::shared_ptr<const unsigned char>& cleanupPhase) const {
    requireOwner();
    if (resumingScope_ && resumingScope_ != this)
        throw ExecutionFailure("await belongs to a different task owner");
    if (captured && (captured->settled || captured != activeExecution_))
        throw ExecutionFailure("await belongs to a different task continuation");
    if (cleanupPhase && (!captured || captured->cleanupDepth == 0 || captured->cleanupPhase != cleanupPhase))
        throw ExecutionFailure("await belongs to an expired cleanup phase");
}

PIPELANG_MODEL_RUNTIME_INLINE std::shared_ptr<const unsigned char> TaskScope::activeCleanupPhase() const noexcept {
    return cleanupActive() ? activeExecution_->cleanupPhase : nullptr;
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::cleanupActive() const noexcept {
    return activeExecution_ && !activeExecution_->settled && activeExecution_->cleanupDepth != 0;
}

PIPELANG_MODEL_RUNTIME_INLINE bool TaskScope::closingWait(const std::shared_ptr<TaskGroup>& group,
    const std::shared_ptr<Execution>& execution,
    const std::shared_ptr<const unsigned char>& cleanupPhase) const {
    if (stopping_ || (execution && execution->settled))
        return true;
    if (!closing_ && !groupClosing(group))
        return false;
    return !cleanupPhase || !execution || execution->cleanupDepth == 0 || execution->cleanupPhase != cleanupPhase;
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::resume(std::coroutine_handle<> continuation, const std::shared_ptr<TaskGroup>& group,
    InvocationPermit permit, const std::shared_ptr<Execution>& execution) {
    InvocationContextScope invocation(std::move(permit));
    struct RestoreGroup {
        std::shared_ptr<TaskGroup>& active;
        std::shared_ptr<TaskGroup> previous;
        std::size_t& depth;
        std::shared_ptr<Execution>& execution;
        std::shared_ptr<Execution> previousExecution;
        TaskScope* previousScope;
        ~RestoreGroup() {
            active = std::move(previous);
            execution = std::move(previousExecution);
            resumingScope_ = previousScope;
            --depth;
        }
    } restore{activeGroup_, std::exchange(activeGroup_, group), resumeDepth_,
        activeExecution_, std::exchange(activeExecution_, execution), std::exchange(resumingScope_, this)};
    ++resumeDepth_;
    continuation.resume();
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::reap() {
    auto entry = entries_.begin();
    while (entry != entries_.end()) {
        if (entry->handle.done()) {
            InvocationContextScope invocation(entry->permit);
            entry->execution->settled = true;
            entry->handle.destroy();
            entry = entries_.erase(entry);
        } else {
            ++entry;
        }
    }
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::abandonDormant(const std::shared_ptr<TaskGroup>& group) {
    abandoningFrames_ = true;
    auto entry = entries_.begin();
    while (entry != entries_.end()) {
        if (entry->execution->waits == 0 && (!group || belongsTo(entry->group, group))) {
            InvocationContextScope invocation(entry->permit);
            entry->execution->settled = true;
            entry->abandon();
            entry->handle.destroy();
            entry = entries_.erase(entry);
        } else {
            ++entry;
        }
    }
    abandoningFrames_ = false;
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::submit(std::function<void()> execute, std::function<void()> complete,
    const std::shared_ptr<Execution>& execution,
    const std::shared_ptr<const unsigned char>& cleanupPhase) {
    std::lock_guard lock(mutex_);
    if (stopping_ || (closing_ && (!cleanupPhase || !execution || execution->settled ||
        execution->cleanupDepth == 0 || execution->cleanupPhase != cleanupPhase)) ||
        jobs_.size() >= capacity_)
        throw ExecutionFailure("host queue unavailable");
    jobs_.push_back({std::move(execute), std::move(complete)});
    if (!worker_.joinable()) {
        try {
            // Hold the queue lock until startup succeeds: a failed thread
            // construction must leave no job or continuation admitted.
            worker_ = std::thread([this] { work(); });
        } catch (...) {
            jobs_.pop_back();
            throw;
        }
    }
    wake_.notify_one();
}

PIPELANG_MODEL_RUNTIME_INLINE void TaskScope::work() {
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
        // Drop the operation's captures before the owner can settle its completion.
        job.execute = nullptr;
        {
            std::lock_guard lock(mutex_);
            completed_.push_back(std::move(job.complete));
        }
        wake_.notify_all();
    }
}

}
