// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ManualWiring.hpp"

namespace di_tests::manual {
class ManualScope;

class ManualRoot final : private RootState, public std::enable_shared_from_this<ManualRoot> {
public:
    static Result<std::shared_ptr<ManualRoot>> Create(std::shared_ptr<ConstructionPolicy> policy) {
        try {
            if (!policy)
                throw pipelang::model::ExecutionFailure("manual host input is null");
            auto owner = pipelang::di::ExecutionOwner::Create();
            if (!owner.is_success())
                return Propagate<std::shared_ptr<ManualRoot>>(owner);
            auto execution = owner.value()->rootExecution();
            if (!execution.is_success())
                return Propagate<std::shared_ptr<ManualRoot>>(execution);
            return Result<std::shared_ptr<ManualRoot>>::Success(std::shared_ptr<ManualRoot>(
                new ManualRoot(owner.value(), execution.value(), std::move(policy))));
        } catch (const std::exception& error) {
            return Result<std::shared_ptr<ManualRoot>>::Failure(pipelang::model::safeErrorMessage(error));
        }
    }
    ManualRoot(const ManualRoot&) = delete;
    ManualRoot& operator=(const ManualRoot&) = delete;
    ManualRoot(ManualRoot&&) = delete;
    ManualRoot& operator=(ManualRoot&&) = delete;

    Result<std::shared_ptr<ManualScope>> CreateScope(Contracts::ScopeStamp stamp);
    Result<void> Close() { return owner->close(); }

    Result<ManualHandle<Contracts::IIdentitySequence>> ResolveIIdentitySequence(Token token = {}) {
        auto root = data();
        return ManualWiring::Resolve<Contracts::IIdentitySequence>(root, {}, &cache.sequence, true, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::Sequence(stage, execution); });
    }
    Result<ManualHandle<Contracts::IRootService>> ResolveIRootService(Token token = {}) {
        auto root = data();
        return ManualWiring::Resolve<Contracts::IRootService>(root, {}, &cache.root, true, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::RootService(*root, nullptr, stage, execution); });
    }
    Result<ManualHandle<Contracts::IFaultyService>> ResolveIFaultyService(Token token = {}) {
        auto root = data();
        return ManualWiring::Resolve<Contracts::IFaultyService>(root, {}, &cache.faulty, true, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::Faulty(*root, nullptr, stage, execution); });
    }
    Result<ManualHandle<Roots::RootDependency>> ResolveRootDependency(Token token = {}) {
        auto root = data();
        return ManualWiring::Resolve<Roots::RootDependency>(root, {}, nullptr, true, token,
            [&](Stage& stage, Execution& execution) { return ManualWiring::Dependency(*root, nullptr, stage, execution, true); });
    }

private:
    friend class ManualScope;
    ManualRoot(std::shared_ptr<pipelang::di::ExecutionOwner> executionOwner,
               std::shared_ptr<Context> context, std::shared_ptr<ConstructionPolicy> input) {
        owner = std::move(executionOwner);
        execution = std::move(context);
        policy = std::move(input);
    }
    std::shared_ptr<RootState> data() {
        return std::shared_ptr<RootState>(shared_from_this(), static_cast<RootState*>(this));
    }
};
}
