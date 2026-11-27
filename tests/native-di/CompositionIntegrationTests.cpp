// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ConstructionPolicy.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using Startup = di_fixture::composition::DiFixture::Bootstrap::Startup;
using Stamp = di_fixture::source::DiFixture::Contracts::ScopeStamp;
using Root = Startup::Root;
using Policy = di_tests::ConstructionPolicy;
std::size_t assertions = 0;

template<class T>
concept RootResolvesScopedRequest = requires(T& root) { root.ResolveIRequestService(); };
template<class T>
concept RootResolvesScopedUseCase = requires(T& root) { root.ResolveIUseCase(); };
static_assert(!RootResolvesScopedRequest<Root>);
static_assert(!RootResolvesScopedUseCase<Root>);
static_assert(!std::is_copy_constructible_v<Startup::Root>);
static_assert(!std::is_move_constructible_v<Startup::Root>);
static_assert(!std::is_copy_assignable_v<Startup::Root>);
static_assert(!std::is_move_assignable_v<Startup::Root>);
static_assert(!std::is_copy_constructible_v<Startup::Scope>);
static_assert(!std::is_move_constructible_v<Startup::Scope>);
static_assert(!std::is_copy_assignable_v<Startup::Scope>);
static_assert(!std::is_move_assignable_v<Startup::Scope>);
static_assert(std::is_copy_constructible_v<Startup::IUseCaseHandle>);
static_assert(std::is_move_constructible_v<Startup::IUseCaseHandle>);


void Require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

template<class T>
T Value(const pipelang::model::Result<T>& result, const std::string& message) {
    Require(result.is_success(), message + ": " + result.error());
    return result.value();
}

void Success(const pipelang::model::Result<void>& result, const std::string& message) {
    Require(result.is_success(), message + ": " + result.error());
}

Stamp Context(std::int64_t identity, std::string tenant) {
    Stamp stamp;
    stamp.identity = identity;
    stamp.tenant = std::move(tenant);
    return stamp;
}

void IdentitiesAndLifetimeContexts() {
    Require(Startup::Create(std::shared_ptr<Policy>{}).is_failure(), "null borrowed host input accepted");
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create root");
    auto stampA = Context(101, "tenant-a");
    auto scopeA = Value(root->CreateScope(stampA), "create scope A");
    stampA.identity = 999;
    stampA.tenant = "mutated-caller-value";
    auto scopeB = Value(root->CreateScope(Context(202, "tenant-b")), "create scope B");
    auto singletonA = Value(scopeA->ResolveIRootService(), "root from scope A");
    auto singletonB = Value(scopeB->ResolveIRootService(), "root from scope B");
    const auto rootId = Value(singletonA.Identity(), "root identity A");
    const auto dependencyId = Value(singletonA.DependencyIdentity(), "root dependency");
    Require(rootId == Value(singletonB.Identity(), "root identity B"), "singleton differs across scopes");
    Require(policy->ReadsOf("root") == 1, "singleton constructor repeated");

    auto requestA = Value(scopeA->ResolveIRequestService(), "request A");
    auto requestAgain = Value(scopeA->ResolveIRequestService(), "request A cached");
    auto requestB = Value(scopeB->ResolveIRequestService(), "request B");
    Require(Value(requestA.Identity(), "request A id") == Value(requestAgain.Identity(), "request cached id"), "scoped cache missed");
    Require(Value(requestA.Identity(), "request A id") != Value(requestB.Identity(), "request B id"), "scoped instances shared");
    Require(Value(requestA.ScopeIdentity(), "stamp A") == 101 && Value(requestB.ScopeIdentity(), "stamp B") == 202, "scoped stamp mixed");
    Require(Value(requestA.Tenant(), "tenant A") == "tenant-a" && Value(requestB.Tenant(), "tenant B") == "tenant-b", "snapshot tenant mixed");
    auto echoA = Value(scopeA->ResolveScopeEcho(), "second scoped snapshot consumer");
    Require(Value(echoA.ScopeIdentity(), "echo scope") == 101 && Value(echoA.Tenant(), "echo tenant") == "tenant-a", "snapshot input was moved from or aliased by an earlier consumer");
    auto transientA = Value(scopeA->ResolveIUseCase(), "transient A");
    auto transientAgain = Value(scopeA->ResolveIUseCase(), "transient A again");
    Require(Value(transientA.Identity(), "transient id A") != Value(transientAgain.Identity(), "transient id B"), "transient was cached");
    Require(Value(transientA.RequestIdentity(), "nested request") == Value(requestA.Identity(), "request A id"), "transient got another scoped dependency");
    Require(Value(transientA.RootIdentity(), "nested root") == rootId, "nested root identity changed");
    Success(scopeA->Close(), "close scope A");
    Require(requestA.Identity().is_failure() && transientA.Identity().is_failure(), "closed scope handles remained callable");
    Require(Value(singletonA.Identity(), "root handle survives child") == rootId, "child close invalidated root handle");
    Require(Value(singletonA.DependencyIdentity(), "root transient survives child") == dependencyId, "root captured transient bound to child");
    Require(Value(requestB.Tenant(), "scope B survives A") == "tenant-b", "closing sibling invalidated request");
    Success(scopeB->Close(), "close scope B");
    Success(root->Close(), "close root");
    Require(singletonA.Identity().is_failure(), "closed root handle remained callable");
}

void FailedAndCancelledConstruction() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create failure root");
    auto existing = Value(root->ResolveIRootService(), "prime existing singleton");
    auto sequence = Value(root->ResolveIIdentitySequence(), "identity witness");
    const auto existingId = Value(existing.Identity(), "existing id");
    const auto before = Value(sequence.Count(), "count before failure");
    policy->selectedKind = "faulty";
    policy->decision = Policy::Decision::Failure;
    Require(root->ResolveIFaultyService().is_failure(), "failed constructor became success");
    Require(Value(sequence.Count(), "count after failure") == before, "failed attempt committed source allocations/IDs");
    Require(Value(existing.Identity(), "existing after failure") == existingId, "failed attempt replaced prior singleton");
    policy->decision = Policy::Decision::ThrowCancellation;
    Require(root->ResolveIFaultyService().is_cancelled(), "constructor cancellation lost its outcome");
    Require(Value(sequence.Count(), "count after cancellation") == before, "cancelled attempt committed source IDs");
    policy->decision = Policy::Decision::Success;
    auto recovered = Value(root->ResolveIFaultyService(), "retry faulty service");
    const auto recoveredId = Value(recovered.Identity(), "recovered id");
    auto cached = Value(root->ResolveIFaultyService(), "faulty cached after success");
    Require(Value(cached.Identity(), "cached faulty id") == recoveredId, "successful retry not cached");
    Require(policy->ReadsOf("faulty") == 3, "failed/cancelled attempts were cached or retry reconstructed");
    Require(Value(recovered.RootIdentity(), "recovered dependency") == existingId, "retry lost earlier singleton");
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    Require(root->ResolveIFaultyService(cancellation.token()).is_cancelled(), "cached hit ignored cancellation admission");
    Success(root->Close(), "close failure root");
}

void LastConstructorCallbackCancellation() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create cancel root");
    auto sequence = Value(root->ResolveIIdentitySequence(), "prime sequence");
    pipelang::model::CancellationSource cancellation;
    policy->onRead = [&](const std::string& kind, std::int64_t) {
        if (kind == "root")
            cancellation.cancel();
    };
    Require(root->ResolveIRootService(cancellation.token()).is_cancelled(), "constructor callback cancellation published success");
    Require(Value(sequence.Count(), "count after final cancellation") == 0, "cancelled constructor graph published changes");
    policy->onRead = {};
    Value(root->ResolveIRootService(), "retry after callback cancellation");
    Require(policy->ReadsOf("root") == 2, "cancelled singleton was cached");
    Success(root->Close(), "close cancel root");
}

void DomainResultsCommitBeforeFlattening() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create method root");
    auto scope = Value(root->CreateScope(Context(303, "tenant-c")), "create method scope");
    auto useCase = Value(scope->ResolveIUseCase(), "resolve method service");
    Require(Value(useCase.Echo(1, 2, 3), "authored argument names") == 123, "generated method shadowed source arguments");
    pipelang::model::CancellationSource sourceCancellation;
    sourceCancellation.cancel();
    Require(useCase.ObserveCancellation(sourceCancellation.token()).is_cancelled(), "source token argument was not forwarded");
    Require(useCase.ObserveCancellation({}).is_success(), "default source token failed");
    policy->selectedKind = "operation";
    policy->decision = Policy::Decision::Failure;
    Require(useCase.ReadPolicy().is_failure(), "domain failure not flattened");
    Require(Value(useCase.OperationCount(), "failure operation count") == 1, "domain failure rolled back authoritative method result");
    policy->decision = Policy::Decision::Cancelled;
    Require(useCase.ReadPolicy().is_cancelled(), "domain cancellation not flattened");
    Require(Value(useCase.OperationCount(), "cancelled operation count") == 2, "returned domain cancellation rolled back source result");
    policy->decision = Policy::Decision::Success;
    pipelang::model::CancellationSource cancellation;
    policy->onRead = [&](const std::string& kind, std::int64_t) {
        if (kind == "operation")
            cancellation.cancel();
    };
    Require(useCase.ReadPolicy(cancellation.token()).is_success(), "late method cancellation relabeled authoritative success");
    Require(Value(useCase.OperationCount(), "late successful operation count") == 3, "late successful result did not commit");
    policy->onRead = {};
    Success(scope->Close(), "close method scope");
    Success(root->Close(), "close method root");
}

void MovedHandleRefusal() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create move root");
    auto scope = Value(root->CreateScope(Context(505, "move")), "create move scope");
    auto original = Value(scope->ResolveIUseCase(), "original move handle");
    auto moved = std::move(original);
    const auto before = Value(moved.OperationCount(), "count before moved-from calls");
    Require(original.Identity().is_failure(), "moved-from primitive method accepted");
    Require(original.ReadPolicy().is_failure(), "moved-from Result method accepted");
    Require(Value(moved.OperationCount(), "count after moved-from calls") == before, "moved-from call executed source");
    Require(moved.Identity().is_success(), "moved-to handle invalid");
    Success(scope->Close(), "close move scope");
    Success(root->Close(), "close move root");
}

void CloseReentryDoesNotMutateLifecycle() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create close-reentry root");
    bool rootCloseRejected = false;
    policy->onRead = [&](const std::string& kind, std::int64_t) {
        if (kind == "root")
            rootCloseRejected = root->Close().is_failure();
    };
    auto singleton = Value(root->ResolveIRootService(), "constructor survives denied close callback");
    Require(rootCloseRejected, "constructor callback closed root");
    Require(singleton.Identity().is_success(), "denied close changed root lifecycle");
    policy->onRead = {};
    auto scope = Value(root->CreateScope(Context(404, "close-reentry")), "create close-reentry scope");
    auto useCase = Value(scope->ResolveIUseCase(), "resolve close-reentry method");
    bool scopeCloseRejected = false;
    policy->onRead = [&](const std::string& kind, std::int64_t) {
        if (kind == "operation")
            scopeCloseRejected = scope->Close().is_failure();
    };
    Require(useCase.ReadPolicy().is_success(), "method failed after denied close callback");
    Require(scopeCloseRejected && useCase.OperationCount().is_success(), "denied scope close changed lifecycle");
    policy->onRead = {};
    Success(scope->Close(), "ordinary scope close after callback");
    Success(root->Close(), "ordinary root close after callback");
}

void ScopeChurnReclaimsClosedGraphs() {
    auto policy = std::make_shared<Policy>();
    policy->trace = false;
    auto root = Value(Startup::Create(policy), "create churn root");
    auto sequence = Value(root->ResolveIIdentitySequence(), "churn identity witness");
    const auto before = Value(sequence.Count(), "churn count before");
    for (std::int64_t iteration = 0; iteration < 2200; ++iteration) {
        // At most one child graph is live. The cumulative two-per-scope service
        // count deliberately exceeds the domain's 4096 simultaneous-cell cap.
        auto scope = Value(root->CreateScope(Context(iteration + 1, "churn")), "create churn scope");
        auto useCase = Value(scope->ResolveIUseCase(), "resolve churn service");
        Require(Value(useCase.ScopeIdentity(), "churn stamp") == iteration + 1, "churn reused another scope");
        Success(scope->Close(), "close churn scope");
    }
    Require(Value(sequence.Count(), "churn count after") - before >= 4400, "churn did not exceed cumulative cell capacity");
    Success(root->Close(), "close churn root");
}

void ReentryAndWrongOwner() {
    auto policy = std::make_shared<Policy>();
    auto root = Value(Startup::Create(policy), "create reentry root");
    bool reentryAttempted = false;
    bool reentryRejected = false;
    policy->onRead = [&](const std::string& kind, std::int64_t) {
        if (kind == "root" && !reentryAttempted) {
            reentryAttempted = true;
            reentryRejected = root->ResolveIRootService().is_failure();
        }
    };
    auto singleton = Value(root->ResolveIRootService(), "outer resolution with reentry");
    Require(reentryAttempted && reentryRejected, "callback reentry was not rejected");
    Require(policy->ReadsOf("root") == 1, "reentry caused duplicate construction");
    policy->onRead = {};
    std::atomic<bool> resolveRejected = false;
    std::atomic<bool> methodRejected = false;
    std::thread foreign([&] {
        resolveRejected = root->ResolveIRootService().is_failure();
        methodRejected = singleton.Identity().is_failure();
    });
    foreign.join();
    Require(resolveRejected && methodRejected, "wrong-owner invocation was accepted");
    Require(policy->ReadsOf("root") == 1, "wrong-owner call executed source construction");
    Success(root->Close(), "close reentry root");
}

}

int main() {
    try {
        IdentitiesAndLifetimeContexts();
        FailedAndCancelledConstruction();
        LastConstructorCallbackCancellation();
        DomainResultsCommitBeforeFlattening();
        ReentryAndWrongOwner();
        MovedHandleRefusal();
        CloseReentryDoesNotMutateLifecycle();
        ScopeChurnReclaimsClosedGraphs();
        std::cout << "native DI integration assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native DI integration: " << error.what() << '\n';
        return 1;
    }
}
