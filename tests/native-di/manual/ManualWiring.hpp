// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ManualHandle.hpp"
#include <algorithm>

namespace di_tests::manual {

// Deliberate hand-written wiring of the exact fixture, through checked compiler
// constructor bridges. No service registry, reflection or generated ordinal use.
class ManualWiring {
public:
    template<class T>
    static void Reserve(std::vector<T>& values, std::size_t needed) {
        constexpr auto maximum = pipelang::model::ObjectDomain::maximumCells;
        if (needed > maximum)
            throw pipelang::model::ExecutionFailure("manual retention capacity exceeded");
        if (needed <= values.capacity())
            return;
        const auto grown = values.capacity() > maximum / 2 ? maximum : std::max<std::size_t>(8, values.capacity() * 2);
        values.reserve(std::max(needed, grown));
    }

    template<class T>
    static void Prepare(std::vector<T>& retained, std::vector<T>& pending) {
        constexpr auto maximum = pipelang::model::ObjectDomain::maximumCells;
        if (pending.size() >= maximum || retained.size() > maximum - pending.size() - 1)
            throw pipelang::model::ExecutionFailure("manual retention capacity exceeded");
        Reserve(retained, retained.size() + pending.size() + 1);
        Reserve(pending, pending.size() + 1);
    }

    template<class T>
    static void Append(std::vector<T>& retained, std::vector<T>& pending) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<T>);
        if (retained.capacity() - retained.size() < pending.size())
            std::terminate();
        for (auto& value : pending)
            retained.push_back(std::move(value));
    }

    static void Publish(RootState& root, ScopeState* scope, Stage& stage) noexcept {
        static_assert(std::is_nothrow_swappable_v<RootCache>);
        static_assert(std::is_nothrow_swappable_v<ScopeCache>);
        using std::swap;
        swap(root.cache, stage.root);
        Append(root.retained.useCases, stage.rootPending.useCases);
        Append(root.retained.dependencies, stage.rootPending.dependencies);
        if (scope) {
            swap(scope->cache, stage.scope);
            Append(scope->retained.useCases, stage.scopePending.useCases);
            Append(scope->retained.dependencies, stage.scopePending.dependencies);
        }
    }

    static Contracts::IIdentitySequence Sequence(Stage& stage, Execution& execution) {
        if (!stage.root.sequence)
            stage.root.sequence = Contracts::IIdentitySequence(di_fixture::construction::Construct<Roots::IdentitySequence>(execution));
        return *stage.root.sequence;
    }

    static Roots::RootDependency Dependency(RootState& root, ScopeState* scope, Stage& stage,
                                           Execution& execution, bool rootOwned) {
        auto& retained = rootOwned ? root.retained.dependencies : scope->retained.dependencies;
        auto& pending = rootOwned ? stage.rootPending.dependencies : stage.scopePending.dependencies;
        Prepare(retained, pending);
        auto sequence = Sequence(stage, execution);
        auto value = di_fixture::construction::Construct<Roots::RootDependency>(execution, root.policy, sequence);
        pending.push_back(value);
        return value;
    }

    static Contracts::IRootService RootService(RootState& root, ScopeState* scope, Stage& stage, Execution& execution) {
        if (!stage.root.root) {
            auto sequence = Sequence(stage, execution);
            auto child = Dependency(root, scope, stage, execution, true);
            stage.root.root = Contracts::IRootService(di_fixture::construction::Construct<Roots::RootService>(execution, root.policy, sequence, child));
        }
        return *stage.root.root;
    }

    static Contracts::IRequestService Request(RootState& root, ScopeState& scope, Stage& stage, Execution& execution) {
        if (!stage.scope.request) {
            auto sequence = Sequence(stage, execution);
            auto service = RootService(root, &scope, stage, execution);
            stage.scope.request = Contracts::IRequestService(di_fixture::construction::Construct<Requests::RequestService>(execution, root.policy, sequence, scope.stamp, service));
        }
        return *stage.scope.request;
    }

    static Contracts::IUseCase UseCase(RootState& root, ScopeState& scope, Stage& stage, Execution& execution) {
        Prepare(scope.retained.useCases, stage.scopePending.useCases);
        auto sequence = Sequence(stage, execution);
        auto request = Request(root, scope, stage, execution);
        auto value = Contracts::IUseCase(di_fixture::construction::Construct<Application::UseCase>(execution, root.policy, sequence, request));
        stage.scopePending.useCases.push_back(value);
        return value;
    }

    static Contracts::IFaultyService Faulty(RootState& root, ScopeState* scope, Stage& stage, Execution& execution) {
        if (!stage.root.faulty) {
            auto sequence = Sequence(stage, execution);
            auto service = RootService(root, scope, stage, execution);
            auto child = Dependency(root, scope, stage, execution, true);
            stage.root.faulty = Contracts::IFaultyService(di_fixture::construction::Construct<Application::FaultyService>(execution, root.policy, sequence, service, child));
        }
        return *stage.root.faulty;
    }

    static Requests::ScopeEcho Echo(ScopeState& scope, Stage& stage, Execution& execution) {
        if (!stage.scope.echo)
            stage.scope.echo = di_fixture::construction::Construct<Requests::ScopeEcho>(execution, scope.stamp);
        return *stage.scope.echo;
    }

    template<class Reference, class Function>
    static Result<ManualHandle<Reference>> Resolve(std::shared_ptr<RootState> root, std::shared_ptr<ScopeState> scope,
        const std::optional<Reference>* cached, bool rootHandle, Token cancellation, Function&& construct) {
        const auto context = scope ? scope->execution : root->execution;
        const auto open = context->checkOpen(cancellation);
        if (!open.is_success())
            return Propagate<ManualHandle<Reference>>(open);
        auto boundScope = rootHandle ? std::shared_ptr<ScopeState>{} : scope;
        if (cached && *cached)
            return Result<ManualHandle<Reference>>::Success(ManualHandle<Reference>(root, boundScope, **cached));
        std::optional<Stage> stage;
        const auto outcome = context->invoke<Reference>(cancellation, [&](Execution& execution) {
            stage.emplace(Stage{root->cache, scope ? scope->cache : ScopeCache{}, {}, {}});
            auto value = construct(*stage, execution);
            if (cancellation.is_cancelled())
                throw pipelang::model::OperationCancelled("manual construction cancelled");
            return value;
        });
        if (!outcome.is_success())
            return Propagate<ManualHandle<Reference>>(outcome);
        Publish(*root, scope.get(), *stage);
        return Result<ManualHandle<Reference>>::Success(ManualHandle<Reference>(root, boundScope, outcome.value()));
    }
};
}
