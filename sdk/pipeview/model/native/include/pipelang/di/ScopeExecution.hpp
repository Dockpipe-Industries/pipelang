// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ExecutionOwner.hpp"
#include "CleanupSession.hpp"
#include <functional>
#include <type_traits>

namespace pipelang::di {
class ScopeExecution : public std::enable_shared_from_this<ScopeExecution> {
public:
    ScopeExecution(const ScopeExecution&) = delete;
    ScopeExecution& operator=(const ScopeExecution&) = delete;
    ~ScopeExecution();

    model::Result<void> checkOpen(model::CancellationToken cancellation = {}) const {
        try {
            owner_->requireEntry();
            for (auto current = context_; current; current = current->parent.lock()) {
                const auto open = current->lifecycle.requireOpen();
                if (!open.is_success())
                    return open;
            }
            if (owner_->failed_)
                return model::Result<void>::Failure("di.construction_cleanup_pending");
            if (cancellation.is_cancelled())
                return model::Result<void>::Cancelled("di.execution_cancelled");
            return model::Result<void>::Success();
        } catch (const std::exception& error) {
            return model::Result<void>::Failure(model::safeErrorMessage(error));
        }
    }

    template<class T, class Function>
    model::Result<T> invoke(model::CancellationToken cancellation, Function&& function) {
        static_assert(std::is_void_v<T> || std::is_nothrow_move_constructible_v<T>);
        const auto available = checkOpen(cancellation);
        if (available.is_cancelled())
            return model::Result<T>::Cancelled(available.error());
        if (!available.is_success())
            return model::Result<T>::Failure(available.error());
        const auto keepAlive = shared_from_this();
        const auto admitted = context_->lifecycle.beginOperation();
        if (!admitted.is_success())
            return model::Result<T>::Failure(admitted.error());
        OperationCompletion completion(*owner_, *context_, admitted.value(), false);
        try {
            // Ordinary calls retain the established stack transaction path. No
            // construction identity, journal scan or retention heap is involved.
            model::GraphTransaction graph(owner_->objects_.domain());
            model::EvaluationBudget budget;
            budget.setCancellationCheck([&cancellation] { return cancellation.is_cancelled(); });
            model::ObjectExecution execution(owner_->tasks_, budget, graph, {});
            execution.cancellation = cancellation;
            if constexpr (std::is_void_v<T>) {
                std::invoke(std::forward<Function>(function), execution);
                graph.commit();
                return model::Result<void>::Success();
            } else {
                T value = std::invoke(std::forward<Function>(function), execution);
                using model::validate;
                validate(value);
                if constexpr (!std::is_same_v<T, typename model::ObjectStorage<T>::Stored>)
                    (void)model::ObjectStorage<T>::encode(value, owner_->objects_.domain());
                graph.commit();
                return model::Result<T>::Success(std::move(value));
            }
        } catch (const model::OperationCancelled& error) {
            return model::Result<T>::Cancelled(model::safeErrorMessage(error));
        } catch (const std::exception& error) {
            return model::Result<T>::Failure(model::safeErrorMessage(error));
        }
    }
    template<class T, class Function>
    model::Result<T> construct(model::CancellationToken cancellation, Function&& function) {
        return execute<T>(std::move(cancellation), true, std::forward<Function>(function));
    }

    template<class T, class Function>
    model::Result<model::Task<model::Result<T>>> start(model::CancellationToken cancellation,
                                                     Function function) {
        using Admission = model::Result<model::Task<model::Result<T>>>;
        const auto available = checkOpen(cancellation);
        if (available.is_cancelled())
            return Admission::Cancelled(available.error());
        if (!available.is_success())
            return Admission::Failure(available.error());
        const auto keepAlive = shared_from_this();
        owner_->invocationActive_ = true;
        struct Exit { bool& active; ~Exit() { active = false; } } exit{owner_->invocationActive_};
        try {
            // A task frame owns its closure and context, never its executor.
            return owner_->tasks_.template tryStart<model::Result<T>>(context_->tasks,
                [owner = owner_.get(), context = context_, cancellation = std::move(cancellation),
                 function = std::move(function)](model::CancellationToken taskCancellation) mutable {
                    return runAsync<T>(owner, std::move(context),
                        model::CancellationToken::combine(std::move(cancellation), taskCancellation),
                        std::move(function));
                });
        } catch (const model::OperationCancelled& error) {
            return Admission::Cancelled(model::safeErrorMessage(error));
        } catch (const std::exception& error) {
            return Admission::Failure(model::safeErrorMessage(error));
        }
    }

    model::Result<void> close() {
        try {
            const auto keepAlive = shared_from_this();
            owner_->requireEntry();
            return owner_->closeContext(context_);
        } catch (const std::exception& error) {
            return model::Result<void>::Failure(model::safeErrorMessage(error));
        }
    }
    model::Result<CleanupStatus> cleanupStatus() const {
        try {
            owner_->tasks_.requireOwner();
            return model::Result<CleanupStatus>::Success(ExecutionOwner::statusOf(context_));
        } catch (const std::exception& error) {
            return model::Result<CleanupStatus>::Failure(model::safeErrorMessage(error));
        }
    }
    model::Result<CleanupSession> cleanupSession() const {
        try {
            owner_->tasks_.requireOwner();
            return model::Result<CleanupSession>::Success(CleanupSession(context_));
        } catch (const std::exception& error) {
            return model::Result<CleanupSession>::Failure(model::safeErrorMessage(error));
        }
    }
    model::Result<std::size_t> poll() { return owner_->poll(); }
    model::Result<std::size_t> pending() const {
        try {
            owner_->tasks_.requireOwner();
            return model::Result<std::size_t>::Success(owner_->tasks_.pending(context_->tasks));
        } catch (const std::exception& error) {
            return model::Result<std::size_t>::Failure(model::safeErrorMessage(error));
        }
    }
    model::Result<model::Task<model::Result<void>>> closeAsync() {
        try {
            owner_->requireEntry();
            if (!context_->closeResult)
                context_->closeResult = std::make_shared<model::TaskState<model::Result<void>>>();
            const auto result = owner_->closeContext(context_);
            if (context_->lifecycle.status().value().phase == ScopePhase::Closed)
                context_->closeResult->result = result;
            return model::Result<model::Task<model::Result<void>>>::Success(
                model::Task<model::Result<void>>(context_->closeResult));
        } catch (const std::exception& error) {
            return model::Result<model::Task<model::Result<void>>>::Failure(model::safeErrorMessage(error));
        }
    }

private:
    friend class ExecutionOwner;
    friend class ConstructionAttempt;
    ScopeExecution(std::shared_ptr<ExecutionOwner> owner,
                   std::shared_ptr<detail::ScopeContext> context)
        : owner_(std::move(owner)), context_(std::move(context)) {}

    template<class T, class Function>
    static model::Coroutine<model::Result<T>> runAsync(
        ExecutionOwner* owner, std::shared_ptr<detail::ScopeContext> context,
        model::CancellationToken cancellation, Function function) {
        const auto admitted = context->lifecycle.beginOperation();
        if (!admitted.is_success())
            co_return model::Result<T>::Failure(admitted.error());
        struct Finish {
            detail::ScopeContext& context;
            ScopeTicket ticket;
            ~Finish() {
                if (!context.lifecycle.finishOperation(ticket).is_success())
                    std::terminate();
            }
        } finish{*context, admitted.value()};
        try {
            model::GraphTransaction graph(owner->objects_.domain());
            model::EvaluationBudget budget;
            // Match ordinary source async: cancellation is cooperative through
            // authored token checks and await/host boundaries. An automatic
            // expression-step check would interrupt publication of an already
            // authoritative host result before the source can handle it.
            model::ObjectExecution execution(owner->tasks_, budget, graph, {});
            execution.cancellation = cancellation;
            auto result = co_await std::invoke(function, execution, cancellation);
            using model::validate;
            validate(result);
            using Carrier = model::Result<T>;
            if constexpr (!std::is_same_v<Carrier, typename model::ObjectStorage<Carrier>::Stored>)
                (void)model::ObjectStorage<Carrier>::encode(result, owner->objects_.domain());
            // Preserve a returned authoritative outcome after external work.
            graph.commit();
            co_return result;
        } catch (const model::OperationCancelled& error) {
            co_return model::Result<T>::Cancelled(model::safeErrorMessage(error));
        } catch (const std::exception& error) {
            co_return model::Result<T>::Failure(model::safeErrorMessage(error));
        } catch (...) {
            co_return model::Result<T>::Failure("di.unexpected_async_failure");
        }
    }

    class OperationCompletion {
    public:
        OperationCompletion(ExecutionOwner& owner, detail::ScopeContext& context,
                            ScopeTicket ticket, bool construction)
            : owner_(owner), context_(context), ticket_(ticket), construction_(construction) {
            owner_.invocationActive_ = true;
        }
        ~OperationCompletion() {
            const auto finished = construction_ ? context_.lifecycle.finishConstruction(ticket_)
                                                : context_.lifecycle.finishOperation(ticket_);
            if (!finished.is_success())
                std::terminate();
            owner_.invocationActive_ = false;
        }
    private:
        ExecutionOwner& owner_;
        detail::ScopeContext& context_;
        ScopeTicket ticket_;
        bool construction_;
    };

    template<class T, class Function>
    model::Result<T> execute(model::CancellationToken cancellation, bool construction, Function&& function) {
        static_assert(std::is_void_v<T> || std::is_nothrow_move_constructible_v<T>);
        const auto available = checkOpen(cancellation);
        if (available.is_cancelled())
            return model::Result<T>::Cancelled(available.error());
        if (!available.is_success())
            return model::Result<T>::Failure(available.error());
        const auto keepAlive = shared_from_this();
        const auto admitted = construction ? context_->lifecycle.beginConstruction()
                                           : context_->lifecycle.beginOperation();
        if (!admitted.is_success())
            return model::Result<T>::Failure(admitted.error());
        OperationCompletion completion(*owner_, *context_, admitted.value(), construction);
        if (owner_->nextAttempt_ == std::numeric_limits<std::uint64_t>::max())
            return model::Result<T>::Failure("di.construction_identity_exhausted");
        const auto attemptId = owner_->nextAttempt_++;
        std::unique_ptr<model::GraphTransaction> graph;
        try {
            graph = std::make_unique<model::GraphTransaction>(owner_->objects_.domain());
            model::EvaluationBudget budget;
            budget.setCancellationCheck([&cancellation] { return cancellation.is_cancelled(); });
            model::ObjectExecution execution(owner_->tasks_, budget, *graph, {});
            execution.cancellation = cancellation;
            ConstructionAttempt attempt(*owner_, owner_->journal_, *graph, attemptId);
            if constexpr (std::is_void_v<T>) {
                std::invoke(std::forward<Function>(function), execution, attempt);
                if (construction && cancellation.is_cancelled())
                    throw model::OperationCancelled("di.construction_cancelled");
                graph->commit();
                owner_->journal_.publish(attemptId);
                return model::Result<void>::Success();
            } else {
                T value = std::invoke(std::forward<Function>(function), execution, attempt);
                using model::validate;
                validate(value);
                if constexpr (!std::is_same_v<T, typename model::ObjectStorage<T>::Stored>)
                    (void)model::ObjectStorage<T>::encode(value, owner_->objects_.domain());
                if (construction && cancellation.is_cancelled())
                    throw model::OperationCancelled("di.construction_cancelled");
                graph->commit();
                owner_->journal_.publish(attemptId);
                return model::Result<T>::Success(std::move(value));
            }
        } catch (const model::OperationCancelled& error) {
            retainAndUnwind(std::move(graph), attemptId);
            return model::Result<T>::Cancelled(model::safeErrorMessage(error));
        } catch (const std::exception& error) {
            retainAndUnwind(std::move(graph), attemptId);
            return model::Result<T>::Failure(model::safeErrorMessage(error));
        } catch (...) {
            retainAndUnwind(std::move(graph), attemptId);
            return model::Result<T>::Failure("di.unexpected_construction_failure");
        }
    }
    void retainAndUnwind(std::unique_ptr<model::GraphTransaction> graph, std::uint64_t attempt) {
        if (!graph)
            return;
        owner_->failed_.emplace(ExecutionOwner::FailedConstruction{std::move(graph), attempt});
        model::EvaluationBudget budget;
        (void)owner_->unwindFailed(budget);
    }
    std::shared_ptr<ExecutionOwner> owner_;
    std::shared_ptr<detail::ScopeContext> context_;
};

template<class Ref, class Cleanup>
ResourceJournal::Reservation<Ref> ConstructionAttempt::reserve(
    const std::shared_ptr<ScopeExecution>& scope, std::string key, Cleanup cleanup) {
    if (!scope || scope->owner_.get() != &owner_)
        throw model::ExecutionFailure("di.execution_foreign_owner");
    return journal_.reserve<Ref>(scope->context_, identity_, std::move(key), std::move(cleanup));
}

inline void ExecutionOwner::requireEntry() const {
    tasks_.requireOwner();
    if (invocationActive_)
        throw model::ExecutionFailure("di.execution_reentry");
}
inline bool ExecutionOwner::within(const std::shared_ptr<detail::ScopeContext>& candidate,
                                   const std::shared_ptr<detail::ScopeContext>& ancestor) {
    for (auto current = candidate; current; current = current->parent.lock()) {
        if (current == ancestor)
            return true;
    }
    return false;
}
inline void ExecutionOwner::beginClosing(const std::shared_ptr<detail::ScopeContext>& context) {
    context->closeRequested = true;
    (void)context->lifecycle.beginClose();
    for (const auto& child : context->children)
        beginClosing(child);
}
inline void ExecutionOwner::finishClosing(const std::shared_ptr<detail::ScopeContext>& context) {
    for (const auto& child : context->children)
        finishClosing(child);
    if (context->cleanup.pending != 0)
        return;
    const auto closed = context->lifecycle.finishClose();
    (void)closed;
    if (context->lifecycle.status().value().phase != ScopePhase::Closed)
        return;
    context->cleanup.phase = ScopePhase::Closed;
    if (!context->parentReleased && context->parentTicket) {
        if (const auto parent = context->parent.lock())
            (void)parent->lifecycle.finishChild(*context->parentTicket);
        if (context->rootTicket) {
            auto root = context->parent.lock();
            while (const auto ancestor = root->parent.lock())
                root = ancestor;
            (void)root->lifecycle.finishChild(*context->rootTicket);
        }
        context->parentReleased = true;
    }
    if (!context->diagnosticsRolledUp) {
        if (const auto parent = context->parent.lock()) {
            const auto nested = statusOf(context);
            parent->cleanup.attempted += nested.attempted;
            parent->cleanup.succeeded += nested.succeeded;
            parent->cleanup.failed += nested.failed;
            parent->cleanup.cancelled += nested.cancelled;
            parent->cleanup.unknown += nested.unknown;
            context->diagnosticsRolledUp = true;
        }
    }
    if (context->closeResult)
        context->closeResult->result = cleanupOutcome(statusOf(context));
}
inline CleanupStatus ExecutionOwner::statusOf(const std::shared_ptr<detail::ScopeContext>& context) {
    return CleanupSession::snapshot(context);
}
inline model::Result<void> ExecutionOwner::cleanupOutcome(const CleanupStatus& status) {
    if (status.pending != 0)
        return model::Result<void>::Failure("di.cleanup_pending");
    if (status.failed != 0)
        return model::Result<void>::Failure("di.cleanup_failed_needs_reconciliation");
    if (status.cancelled != 0)
        return model::Result<void>::Cancelled("di.cleanup_cancelled_needs_reconciliation");
    return model::Result<void>::Success();
}
inline bool ExecutionOwner::unwindFailed(model::EvaluationBudget& budget) {
    if (!failed_)
        return true;
    model::ObjectExecution execution(tasks_, budget, *failed_->graph, {});
    const auto attempt = failed_->attempt;
    const bool complete = journal_.cleanup(execution, {}, [attempt](const auto&, auto candidate, bool published) {
        return !published && candidate == attempt;
    });
    journal_.eraseFinished(attempt);
    if (complete)
        failed_.reset(); // Unwind cleanup runs before the provisional graph disappears.
    return complete;
}
inline model::Result<void> ExecutionOwner::closeContext(
    const std::shared_ptr<detail::ScopeContext>& context) {
    if (context->lifecycle.status().value().phase == ScopePhase::Closed)
        return cleanupOutcome(statusOf(context));
    beginClosing(context);
    tasks_.closeGroup(context->tasks);
    if (tasks_.pending(context->tasks) != 0)
        return model::Result<void>::Failure("di.execution_tasks_pending");
    invocationActive_ = true;
    struct Exit { bool& active; ~Exit() { active = false; } } exit{invocationActive_};
    model::EvaluationBudget budget;
    if (!unwindFailed(budget))
        return model::Result<void>::Failure("di.cleanup_pending");
    model::GraphTransaction graph(objects_.domain());
    model::ObjectExecution execution(tasks_, budget, graph, {});
    const bool complete = journal_.cleanup(execution, {}, [&](const auto& scope, auto, bool) {
        return within(scope, context);
    });
    try {
        graph.commit();
    } catch (...) {
        ++context->cleanup.failed;
        ++context->cleanup.unknown;
    }
    journal_.eraseFinished();
    if (!complete)
        return model::Result<void>::Failure("di.cleanup_pending");
    finishClosing(context);
    const auto result = cleanupOutcome(statusOf(context));
    if (context->closeResult && context->cleanup.phase == ScopePhase::Closed)
        context->closeResult->result = result;
    if (context == root_ && context->cleanup.phase == ScopePhase::Closed && !domainClosed_) {
        objects_.domain()->close();
        domainClosed_ = true;
    }
    return result;
}
inline model::Result<void> ExecutionOwner::close() {
    try {
        const auto keepAlive = shared_from_this();
        requireEntry();
        return closeContext(root_);
    } catch (const std::exception& error) {
        return model::Result<void>::Failure(model::safeErrorMessage(error));
    }
}
inline model::Result<std::size_t> ExecutionOwner::poll() {
    try {
        const auto keepAlive = shared_from_this();
        requireEntry();
        invocationActive_ = true;
        std::size_t count;
        try {
            count = tasks_.poll();
        } catch (...) {
            invocationActive_ = false;
            throw;
        }
        invocationActive_ = false;
        processDeferred(root_);
        return model::Result<std::size_t>::Success(count);
    } catch (const std::exception& error) {
        return model::Result<std::size_t>::Failure(model::safeErrorMessage(error));
    }
}
inline void ExecutionOwner::processDeferred(const std::shared_ptr<detail::ScopeContext>& context) {
    if (context->closeRequested) {
        (void)closeContext(context);
        return;
    }
    for (const auto& child : context->children)
        processDeferred(child);
    context->children.erase(std::remove_if(context->children.begin(), context->children.end(), [](const auto& child) {
        return child->diagnosticsRolledUp;
    }), context->children.end());
}
inline ExecutionOwner::~ExecutionOwner() {
    if (thread_ != std::this_thread::get_id() || invocationActive_)
        std::terminate();
    try {
        invocationActive_ = true;
        tasks_.shutdown();
        invocationActive_ = false;
        // Each bounded session either attempts an entry or completes. Callbacks
        // cannot replay; no pending resource is dropped with the owner graph.
        for (std::size_t session = 0; session <= ScopeLimits::maximumEntries; ++session) {
            (void)closeContext(root_);
            if (root_->cleanup.phase == ScopePhase::Closed)
                break;
        }
    } catch (...) {
        std::terminate();
    }
}
inline ScopeExecution::~ScopeExecution() {
    if (owner_->thread_ != std::this_thread::get_id())
        std::terminate();
    context_->closeRequested = true;
    if (!owner_->invocationActive_) {
        try {
            owner_->tasks_.requireOwner();
            owner_->invocationActive_ = true;
            owner_->tasks_.drainGroup(context_->tasks);
            owner_->invocationActive_ = false;
            for (std::size_t session = 0; session <= ScopeLimits::maximumEntries; ++session) {
                (void)owner_->closeContext(context_);
                if (context_->cleanup.phase == ScopePhase::Closed)
                    break;
            }
        } catch (...) {
            owner_->invocationActive_ = false;
            // A host callback may release the last facade. The owner boundary
            // processes this request after callback reentry protection unwinds.
        }
    }
}
inline model::Result<std::shared_ptr<ScopeExecution>> ExecutionOwner::rootExecution() {
    using Context = model::Result<std::shared_ptr<ScopeExecution>>;
    try {
        requireEntry();
        const auto open = root_->lifecycle.requireOpen();
        if (!open.is_success())
            return Context::Failure(open.error());
        if (auto existing = rootHandle_.lock())
            return Context::Success(std::move(existing));
        auto handle = std::shared_ptr<ScopeExecution>(new ScopeExecution(shared_from_this(), root_));
        rootHandle_ = handle;
        return Context::Success(std::move(handle));
    } catch (const std::exception& error) {
        return Context::Failure(model::safeErrorMessage(error));
    }
}
inline model::Result<std::shared_ptr<ScopeExecution>> ExecutionOwner::createScope(
    std::shared_ptr<ScopeExecution> parent, ScopeLimits limits) {
    using Context = model::Result<std::shared_ptr<ScopeExecution>>;
    try {
        requireEntry();
        if (parent && parent->owner_.get() != this)
            return Context::Failure("di.execution_foreign_owner");
        const auto ancestor = parent ? parent->context_ : root_;
        for (auto current = ancestor; current; current = current->parent.lock()) {
            const auto open = current->lifecycle.requireOpen();
            if (!open.is_success())
                return Context::Failure(open.error());
        }
        if (nextScope_ == std::numeric_limits<std::uint64_t>::max())
            return Context::Failure("di.scope_identity_exhausted");
        auto context = std::make_shared<detail::ScopeContext>(nextScope_, limits);
        context->parent = ancestor;
        context->tasks = tasks_.createGroup(ancestor->tasks);
        ancestor->children.erase(std::remove_if(ancestor->children.begin(), ancestor->children.end(), [](const auto& child) {
            return child->diagnosticsRolledUp;
        }), ancestor->children.end());
        ancestor->children.reserve(ancestor->children.size() + 1);
        const auto rootAdmission = root_->lifecycle.beginChild();
        if (!rootAdmission.is_success())
            return Context::Failure(rootAdmission.error());
        const auto admitted = ancestor == root_ ? rootAdmission : ancestor->lifecycle.beginChild();
        if (!admitted.is_success()) {
            (void)root_->lifecycle.finishChild(rootAdmission.value());
            return Context::Failure(admitted.error());
        }
        context->parentTicket = admitted.value();
        if (ancestor != root_)
            context->rootTicket = rootAdmission.value();
        try {
            auto handle = std::shared_ptr<ScopeExecution>(new ScopeExecution(shared_from_this(), context));
            ancestor->children.push_back(context);
            ++nextScope_;
            return Context::Success(std::move(handle));
        } catch (...) {
            (void)ancestor->lifecycle.finishChild(admitted.value());
            if (ancestor != root_)
                (void)root_->lifecycle.finishChild(rootAdmission.value());
            throw;
        }
    } catch (const std::exception& error) {
        return Context::Failure(model::safeErrorMessage(error));
    }
}
}
