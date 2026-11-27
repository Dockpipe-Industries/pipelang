// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ManualRoot.hpp"

namespace di_tests::manual {

class ManualScope final : private ScopeState, public std::enable_shared_from_this<ManualScope> {
public:
    ManualScope(const ManualScope&) = delete;
    ManualScope& operator=(const ManualScope&) = delete;
    ManualScope(ManualScope&&) = delete;
    ManualScope& operator=(ManualScope&&) = delete;

    Result<void> Close() {
        auto outcome = execution->close();
        if (outcome.is_success()) {
            cache = ScopeCache{};
            retained = Retained{};
        }
        return outcome;
    }
    Result<ManualHandle<Contracts::IRootService>> ResolveIRootService(Token token = {}) {
        auto scope = data();
        return ManualWiring::Resolve<Contracts::IRootService>(root, scope, &root->cache.root, true, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::RootService(*root, scope.get(), stage, execution); });
    }
    Result<ManualHandle<Contracts::IRequestService>> ResolveIRequestService(Token token = {}) {
        auto scope = data();
        return ManualWiring::Resolve<Contracts::IRequestService>(root, scope, &cache.request, false, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::Request(*root, *scope, stage, execution); });
    }
    Result<ManualHandle<Contracts::IUseCase>> ResolveIUseCase(Token token = {}) {
        auto scope = data();
        return ManualWiring::Resolve<Contracts::IUseCase>(root, scope, nullptr, false, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::UseCase(*root, *scope, stage, execution); });
    }
    Result<ManualHandle<Requests::ScopeEcho>> ResolveScopeEcho(Token token = {}) {
        auto scope = data();
        return ManualWiring::Resolve<Requests::ScopeEcho>(root, scope, &cache.echo, false, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::Echo(*scope, stage, execution); });
    }

private:
    friend class ManualRoot;
    ManualScope(std::shared_ptr<RootState> rootState, Contracts::ScopeStamp context) {
        root = std::move(rootState);
        stamp = std::move(context);
    }
    std::shared_ptr<ScopeState> data() {
        return std::shared_ptr<ScopeState>(shared_from_this(), static_cast<ScopeState*>(this));
    }
};

inline Result<std::shared_ptr<ManualScope>> ManualRoot::CreateScope(Contracts::ScopeStamp stamp) {
    const auto open = execution->checkOpen();
    if (!open.is_success())
        return Propagate<std::shared_ptr<ManualScope>>(open);
    try {
        using pipelang::model::validate;
        validate(stamp);
        auto scope = std::shared_ptr<ManualScope>(new ManualScope(data(), std::move(stamp)));
        auto context = owner->createScope(execution);
        if (!context.is_success())
            return Propagate<std::shared_ptr<ManualScope>>(context);
        scope->execution = context.value();
        return Result<std::shared_ptr<ManualScope>>::Success(std::move(scope));
    } catch (const std::exception& error) {
        return Result<std::shared_ptr<ManualScope>>::Failure(pipelang::model::safeErrorMessage(error));
    }
}
}
