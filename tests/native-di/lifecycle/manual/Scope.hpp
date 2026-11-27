// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Root.hpp"

namespace lifecycle_test::manual {

class Scope final : private ScopeData, public std::enable_shared_from_this<Scope> {
public:
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    Result<Handle<Contracts::IRootResource>> ResolveIRootResource(Token cancellation = {}) {
        auto scope = data();
        return Wiring::Resolve<Contracts::IRootResource>(root, scope, &root->cache.resource, true, cancellation,
            [&](Stage& stage, Execution& execution, Attempt& attempt) {
                return Wiring::RootResource(*root, stage, execution, attempt);
            });
    }

    Result<Handle<Contracts::IScopedResource>> ResolveIScopedResource(Token cancellation = {}) {
        auto scope = data();
        return Wiring::Resolve<Contracts::IScopedResource>(root, scope, &cache.resource, false, cancellation,
            [&](Stage& stage, Execution& execution, Attempt& attempt) {
                return Wiring::ScopedResource(*root, *scope, stage, execution, attempt, cancellation);
            });
    }

    Result<Handle<Contracts::ITransientResource>> ResolveITransientResource(Token cancellation = {}) {
        auto scope = data();
        return Wiring::Resolve<Contracts::ITransientResource>(root, scope, nullptr, false, cancellation,
            [&](Stage& stage, Execution& execution, Attempt& attempt) {
                return Wiring::TransientResource(*root, *scope, stage, execution, attempt, cancellation);
            });
    }

    Result<void> Close() {
        auto outcome = execution->close();
        ClearClosedCache();
        return outcome;
    }

    Result<pipelang::model::Task<Result<void>>> CloseAsync() {
        auto outcome = execution->closeAsync();
        ClearClosedCache();
        return outcome;
    }

    Result<pipelang::di::CleanupStatus> CleanupStatus() {
        auto outcome = execution->cleanupStatus();
        ClearClosedCache();
        return outcome;
    }

    Result<pipelang::di::CleanupSession> ObserveCleanup() const {
        return execution->cleanupSession();
    }

    Result<std::size_t> Poll() {
        auto outcome = execution->poll();
        ClearClosedCache();
        return outcome;
    }

    Result<std::size_t> Pending() const {
        return execution->pending();
    }

private:
    friend class Root;
    Scope(std::shared_ptr<RootData> rootValue, Contracts::ScopeStamp context) {
        root = std::move(rootValue);
        stamp = std::move(context);
    }

    void ClearClosedCache() {
        const auto status = execution->cleanupStatus();
        if (status.is_success() && status.value().phase == pipelang::di::ScopePhase::Closed)
            cache = ScopeCache{};
    }

    std::shared_ptr<ScopeData> data() {
        return std::shared_ptr<ScopeData>(shared_from_this(), static_cast<ScopeData*>(this));
    }
};

inline Result<std::shared_ptr<Scope>> Root::CreateScope(Contracts::ScopeStamp stamp) {
    const auto open = execution->checkOpen();
    if (!open.is_success())
        return Propagate<std::shared_ptr<Scope>>(open);
    try {
        using pipelang::model::validate;
        validate(stamp);
        auto scope = std::shared_ptr<Scope>(new Scope(data(), std::move(stamp)));
        const auto context = owner->createScope(execution);
        if (!context.is_success())
            return Propagate<std::shared_ptr<Scope>>(context);
        scope->execution = context.value();
        return Result<std::shared_ptr<Scope>>::Success(std::move(scope));
    } catch (const std::exception& error) {
        return Result<std::shared_ptr<Scope>>::Failure(pipelang::model::safeErrorMessage(error));
    }
}
}
