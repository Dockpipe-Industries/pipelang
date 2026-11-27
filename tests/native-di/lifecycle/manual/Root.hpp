// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Wiring.hpp"

namespace lifecycle_test::manual {
class Scope;

class Root final : private RootData, public std::enable_shared_from_this<Root> {
public:
    static Result<std::shared_ptr<Root>> Create(std::shared_ptr<ResourcePortAdapter> port) {
        if (!port)
            return Result<std::shared_ptr<Root>>::Failure("manual null borrowed resource port");
        const auto owner = pipelang::di::ExecutionOwner::Create();
        if (!owner.is_success())
            return Propagate<std::shared_ptr<Root>>(owner);
        const auto context = owner.value()->rootExecution();
        if (!context.is_success())
            return Propagate<std::shared_ptr<Root>>(context);
        try {
            return Result<std::shared_ptr<Root>>::Success(std::shared_ptr<Root>(
                new Root(owner.value(), context.value(), std::move(port))));
        } catch (const std::exception& error) {
            return Result<std::shared_ptr<Root>>::Failure(pipelang::model::safeErrorMessage(error));
        }
    }

    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
    Root(Root&&) = delete;
    Root& operator=(Root&&) = delete;

    Result<std::shared_ptr<Scope>> CreateScope(Contracts::ScopeStamp stamp);

    Result<Handle<Contracts::IRootResource>> ResolveIRootResource(Token cancellation = {}) {
        auto root = data();
        return Wiring::Resolve<Contracts::IRootResource>(root, {}, &cache.resource, true, cancellation,
            [&](Stage& stage, Execution& execution, Attempt& attempt) {
                return Wiring::RootResource(*root, stage, execution, attempt);
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
    friend class Scope;
    Root(std::shared_ptr<pipelang::di::ExecutionOwner> ownerValue,
        std::shared_ptr<Context> context, std::shared_ptr<ResourcePortAdapter> portValue) {
        owner = std::move(ownerValue);
        execution = std::move(context);
        port = std::move(portValue);
    }

    void ClearClosedCache() {
        const auto status = execution->cleanupStatus();
        if (status.is_success() && status.value().phase == pipelang::di::ScopePhase::Closed)
            cache = RootCache{};
    }

    std::shared_ptr<RootData> data() {
        return std::shared_ptr<RootData>(shared_from_this(), static_cast<RootData*>(this));
    }
};
}
