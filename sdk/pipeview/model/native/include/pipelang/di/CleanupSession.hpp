// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ScopeContext.hpp"

namespace pipelang::di {
class ScopeExecution;

// Owner-free observation survives final facade/executor destruction. It cannot
// resolve, execute, close or pump work, and does not extend executor authority.
class CleanupSession {
public:
    model::Result<CleanupStatus> status() const {
        const auto lifecycle = context_->lifecycle.status();
        if (!lifecycle.is_success())
            return model::Result<CleanupStatus>::Failure(lifecycle.error());
        return model::Result<CleanupStatus>::Success(snapshot(context_));
    }
    model::Task<model::Result<void>> task() const {
        const auto lifecycle = context_->lifecycle.status();
        if (!lifecycle.is_success())
            throw model::ExecutionFailure(lifecycle.error());
        return model::Task<model::Result<void>>(context_->closeResult);
    }
private:
    friend class ScopeExecution;
    friend class ExecutionOwner;
    explicit CleanupSession(std::shared_ptr<detail::ScopeContext> context) : context_(std::move(context)) {}
    static CleanupStatus snapshot(const std::shared_ptr<detail::ScopeContext>& context) {
        auto result = context->cleanup;
        result.phase = context->lifecycle.status().value().phase;
        for (const auto& child : context->children) {
            if (child->diagnosticsRolledUp)
                continue;
            const auto nested = snapshot(child);
            result.pending += nested.pending;
            result.attempted += nested.attempted;
            result.succeeded += nested.succeeded;
            result.failed += nested.failed;
            result.cancelled += nested.cancelled;
            result.unknown += nested.unknown;
        }
        return result;
    }
    std::shared_ptr<detail::ScopeContext> context_;
};
}
