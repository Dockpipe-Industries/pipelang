// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ConstructionAttempt.hpp"
#include <pipelang/model/object_model.hpp>
#include <memory>

namespace pipelang::di {
class ScopeExecution;

// Shared handles do not grant cross-thread access. Final release is owner-only.
class ExecutionOwner : public std::enable_shared_from_this<ExecutionOwner> {
public:
    static model::Result<std::shared_ptr<ExecutionOwner>> Create(
        model::TaskOptions tasks = {}, ScopeLimits limits = {}) {
        try {
            return model::Result<std::shared_ptr<ExecutionOwner>>::Success(
                std::shared_ptr<ExecutionOwner>(new ExecutionOwner(tasks, limits)));
        } catch (const std::exception& error) {
            return model::Result<std::shared_ptr<ExecutionOwner>>::Failure(model::safeErrorMessage(error));
        }
    }
    ExecutionOwner(const ExecutionOwner&) = delete;
    ExecutionOwner& operator=(const ExecutionOwner&) = delete;
    ~ExecutionOwner();

    model::Result<std::shared_ptr<ScopeExecution>> rootExecution();
    model::Result<std::shared_ptr<ScopeExecution>> createScope(
        std::shared_ptr<ScopeExecution> parent = {}, ScopeLimits limits = {});
    model::Result<void> close();
    model::Result<std::size_t> poll();

private:
    friend class ScopeExecution;
    friend class ConstructionAttempt;
    struct FailedConstruction {
        std::unique_ptr<model::GraphTransaction> graph;
        std::uint64_t attempt = 0;
    };
    ExecutionOwner(model::TaskOptions tasks, ScopeLimits limits)
        : thread_(std::this_thread::get_id()),
          root_(std::make_shared<detail::ScopeContext>(1, limits)),
          journal_(limits.ownedRecords), tasks_(tasks) {
        root_->tasks = tasks_.createGroup();
    }
    void requireEntry() const;
    bool unwindFailed(model::EvaluationBudget& budget);
    model::Result<void> closeContext(const std::shared_ptr<detail::ScopeContext>& context);
    static bool within(const std::shared_ptr<detail::ScopeContext>& candidate,
                       const std::shared_ptr<detail::ScopeContext>& ancestor);
    static void beginClosing(const std::shared_ptr<detail::ScopeContext>& context);
    static void finishClosing(const std::shared_ptr<detail::ScopeContext>& context);
    static CleanupStatus statusOf(const std::shared_ptr<detail::ScopeContext>& context);
    static model::Result<void> cleanupOutcome(const CleanupStatus& status);
    void processDeferred(const std::shared_ptr<detail::ScopeContext>& context);

    const std::thread::id thread_;
    model::ObjectOwner objects_;
    std::shared_ptr<detail::ScopeContext> root_;
    ResourceJournal journal_;
    std::optional<FailedConstruction> failed_;
    std::weak_ptr<ScopeExecution> rootHandle_;
    std::uint64_t nextScope_ = 2;
    std::uint64_t nextAttempt_ = 1;
    bool invocationActive_ = false;
    bool domainClosed_ = false;
    // Explicit shutdown and reverse member destruction drain before graph state.
    model::TaskScope tasks_;
};
}
#include "ScopeExecution.hpp"
