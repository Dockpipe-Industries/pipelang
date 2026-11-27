// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Handle.hpp"
#include "Stage.hpp"

namespace lifecycle_test::manual {

// Exact handwritten wiring for the same source graph. Resource effects stay in
// checked .pipe factory/cleanup methods; the same runtime journal owns them.
class Wiring {
public:
    static Resources::RootDependency Dependency(RootData& root, Stage& stage,
        Execution& execution, Attempt& attempt) {
        auto reservation = attempt.reserve<Resources::RootDependency>(root.execution,
            "DiLifecycle.Resources.RootDependency", Cleanup<Resources::RootDependency>);
        if (!stage.root.dependencyFactory) {
            stage.root.dependencyFactory = lifecycle_fixture::construction::Construct<Factories::RootDependencyFactory>(
                execution, root.port);
        }
        const auto mark = attempt.allocationMark();
        auto resource = UnwrapFactory(
            lifecycle_fixture::construction::SourceMethods<Factories::RootDependencyFactory>::Create(
                execution, *stage.root.dependencyFactory));
        attempt.adopt(reservation, resource, mark);
        return resource;
    }

    static Contracts::IRootResource RootResource(RootData& root, Stage& stage,
        Execution& execution, Attempt& attempt) {
        if (stage.root.resource)
            return *stage.root.resource;
        auto reservation = attempt.reserve<Contracts::IRootResource>(root.execution,
            "DiLifecycle.Contracts.IRootResource", Cleanup<Contracts::IRootResource>);
        if (!stage.root.rootFactory) {
            stage.root.rootFactory = lifecycle_fixture::construction::Construct<Factories::RootFactory>(
                execution, root.port);
        }
        auto dependency = Dependency(root, stage, execution, attempt);
        const auto mark = attempt.allocationMark();
        auto concrete = UnwrapFactory(
            lifecycle_fixture::construction::SourceMethods<Factories::RootFactory>::Create(
                execution, *stage.root.rootFactory, dependency));
        Contracts::IRootResource resource(concrete);
        attempt.adopt(reservation, resource, mark);
        stage.root.resource = resource;
        return resource;
    }

    static Contracts::IScopedResource ScopedResource(RootData& root, ScopeData& scope,
        Stage& stage, Execution& execution, Attempt& attempt, Token cancellation) {
        if (stage.scope.resource)
            return *stage.scope.resource;
        auto reservation = attempt.reserve<Contracts::IScopedResource>(scope.execution,
            "DiLifecycle.Contracts.IScopedResource", Cleanup<Contracts::IScopedResource>);
        if (!stage.root.scopedFactory) {
            stage.root.scopedFactory = lifecycle_fixture::construction::Construct<Factories::ScopedFactory>(
                execution, root.port);
        }
        auto parent = RootResource(root, stage, execution, attempt);
        const auto mark = attempt.allocationMark();
        auto concrete = UnwrapFactory(
            lifecycle_fixture::construction::SourceMethods<Factories::ScopedFactory>::Create(
                execution, *stage.root.scopedFactory, parent, scope.stamp, cancellation));
        Contracts::IScopedResource resource(concrete);
        attempt.adopt(reservation, resource, mark);
        stage.scope.resource = resource;
        return resource;
    }

    static Contracts::ITransientResource TransientResource(RootData& root, ScopeData& scope,
        Stage& stage, Execution& execution, Attempt& attempt, Token cancellation) {
        auto reservation = attempt.reserve<Contracts::ITransientResource>(scope.execution,
            "DiLifecycle.Contracts.ITransientResource", Cleanup<Contracts::ITransientResource>);
        if (!stage.root.transientFactory) {
            stage.root.transientFactory = lifecycle_fixture::construction::Construct<Factories::TransientFactory>(
                execution, root.port);
        }
        auto child = ScopedResource(root, scope, stage, execution, attempt, cancellation);
        const auto mark = attempt.allocationMark();
        auto concrete = UnwrapFactory(
            lifecycle_fixture::construction::SourceMethods<Factories::TransientFactory>::Create(
                execution, *stage.root.transientFactory, child, scope.stamp));
        Contracts::ITransientResource resource(concrete);
        attempt.adopt(reservation, resource, mark);
        return resource;
    }

    template<class Ref, class Factory>
    static Result<Handle<Ref>> Resolve(std::shared_ptr<RootData> root,
        std::shared_ptr<ScopeData> scope, const std::optional<Ref>* cache,
        bool rootLifetime, Token cancellation, Factory&& factory) {
        auto context = scope ? scope->execution : root->execution;
        const auto open = context->checkOpen(cancellation);
        if (!open.is_success())
            return Propagate<Handle<Ref>>(open);
        auto boundScope = rootLifetime ? std::shared_ptr<ScopeData>{} : scope;
        if (cache && *cache)
            return Result<Handle<Ref>>::Success(Handle<Ref>(root, boundScope, **cache));
        std::optional<Stage> stage;
        const auto outcome = context->construct<Ref>(cancellation,
            [&](Execution& execution, Attempt& attempt) {
                stage.emplace(Stage{root->cache, scope ? scope->cache : ScopeCache{}});
                auto resource = factory(*stage, execution, attempt);
                if (cancellation.is_cancelled())
                    throw pipelang::model::OperationCancelled("manual construction cancelled");
                return resource;
            });
        if (!outcome.is_success())
            return Propagate<Handle<Ref>>(outcome);
        static_assert(std::is_nothrow_swappable_v<RootCache> && std::is_nothrow_swappable_v<ScopeCache>);
        using std::swap;
        swap(root->cache, stage->root);
        if (scope)
            swap(scope->cache, stage->scope);
        return Result<Handle<Ref>>::Success(Handle<Ref>(root, boundScope, outcome.value()));
    }
};
}
