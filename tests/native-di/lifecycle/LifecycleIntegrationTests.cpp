// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ExpectedResourceTrace.hpp"
#include "LifecycleAssertions.hpp"
#include "ResourcePortAdapter.hpp"
#include <iostream>
#include <thread>

namespace {
using Startup = lifecycle_fixture::composition::DiLifecycle::Bootstrap::Startup;
using Stamp = lifecycle_fixture::source::DiLifecycle::Contracts::ScopeStamp;
using lifecycle_test::ResourceTrace;
lifecycle_test::LifecycleAssertions checks;

Stamp Context(std::int64_t identity, std::int64_t failure = 0, std::int64_t cleanupWork = 0) {
    Stamp stamp;
    stamp.identity = identity;
    stamp.failureMode = failure;
    stamp.cleanupWork = cleanupWork;
    return stamp;
}

void NormalReverseOwnershipAndBorrowedInput() {
    auto trace = std::make_shared<ResourceTrace>();
    const auto borrowed = checks.Value(trace->Open("borrowed", 0), "open borrowed sentinel");
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create root");
    auto firstScope = checks.Value(root->CreateScope(Context(11)), "create first scope");
    auto first = checks.Value(firstScope->ResolveITransientResource(), "construct first graph");
    auto secondScope = checks.Value(root->CreateScope(Context(22)), "create second scope");
    auto second = checks.Value(secondScope->ResolveITransientResource(), "construct second graph");
    checks.Require(checks.Value(first.Identity(), "first transient") == 5, "unexpected first resource identity");
    checks.Require(checks.Value(second.Identity(), "second transient") == 7, "unexpected second resource identity");
    checks.Success(firstScope->Close(), "close first scope");
    checks.Require(first.Identity().is_failure(), "closed scope handle remained callable");
    checks.Require(second.Identity().is_success(), "first scope close invalidated independent scope");
    checks.Success(root->Close(), "root recursively closes second scope before root graph");
    checks.Require(second.Identity().is_failure(), "root close left descendant handle callable");
    checks.Trace(trace->Events(), lifecycle_test::ExpectedResourceTrace::NormalTwoScopes());
    checks.Require(trace->OpenCount() == 1 && trace->CloseAttempts(borrowed) == 0,
        "DI disposed or lost the independently borrowed resource");
    const auto eventCount = trace->Events().size();
    checks.Success(root->Close(), "repeat successful root close");
    checks.Require(trace->Events().size() == eventCount, "successful cleanup replayed");
    checks.Success(trace->Close(borrowed), "external owner closes borrowed sentinel");
}

void PartialFactoryAndFailedGraphUnwind() {
    for (std::int64_t mode : {1, 2, 3}) {
        auto trace = std::make_shared<ResourceTrace>();
        auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
        auto root = checks.Value(Startup::Create(port), "create partial factory root");
        auto scope = checks.Value(root->CreateScope(Context(31, mode)), "create failing scope");
        auto outcome = scope->ResolveIScopedResource();
        checks.Require(mode == 2 ? outcome.is_cancelled() : outcome.is_failure(),
            "factory failure/cancellation/constructor fault lost its distinct outcome");
        checks.Require(trace->OpenCount() == 0, "failed resolution leaked acquired resources");
        checks.CloseOrder(trace->Events(), {3, 2, 1});
        // Root and dependency cleanup must run before source graph discard:
        // both source methods read their live identity/dependency fields.
        bool rootObserved = false;
        bool dependencyObserved = false;
        for (const auto& event : trace->Events()) {
            if (event.operation == "cleanup-live" && event.resource == 2 && event.dependency == 1)
                rootObserved = true;
            if (event.operation == "cleanup-live" && event.resource == 1 && event.dependency == 1)
                dependencyObserved = true;
        }
        checks.Require(rootObserved && dependencyObserved, "failed graph discarded before typed source cleanup");
        auto recoveredRoot = checks.Value(root->ResolveIRootResource(), "construct root after failed attempt");
        checks.Require(checks.Value(recoveredRoot.Identity(), "recovered root id") == 5,
            "failed attempt published a root cache");
        checks.Success(root->Close(), "close recovered graph");
        checks.Require(trace->OpenCount() == 0, "recovered graph leaked resources");
    }
}

void PreviousCacheSurvivesAndFailedServiceIsNotCached() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create cached root");
    auto cached = checks.Value(root->ResolveIRootResource(), "prime prior root cache");
    auto scope = checks.Value(root->CreateScope(Context(41, 1)), "create repeatedly failing scope");
    checks.Require(scope->ResolveIScopedResource().is_failure(), "first scoped factory unexpectedly succeeded");
    checks.Require(scope->ResolveIScopedResource().is_failure(), "failed scoped service was cached");
    checks.Require(checks.Value(cached.Identity(), "prior cached root") == 2, "prior cached root changed");
    checks.Require(trace->CloseAttempts(1) == 0 && trace->CloseAttempts(2) == 0,
        "failed attempt unwound a previously published cache");
    checks.Require(trace->CloseAttempts(3) == 1 && trace->CloseAttempts(4) == 1,
        "partial factory close replayed or retry skipped acquisition");
    checks.Success(root->Close(), "close prior cached root");
    checks.Require(trace->OpenCount() == 0, "prior cached graph leaked resources");
}

void OwnerFreeObserverSurvivesAutomaticDisposalAndReportsFailure() {
    auto trace = std::make_shared<ResourceTrace>();
    std::weak_ptr<lifecycle_test::ResourcePortAdapter> weakPort;
    auto observer = [&] {
        auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
        weakPort = port;
        auto root = checks.Value(Startup::Create(port), "create observed disposal root");
        auto scope = checks.Value(root->CreateScope(Context(91)), "create observed disposal scope");
        auto service = checks.Value(scope->ResolveITransientResource(), "construct observed disposal graph");
        trace->SetCloseMode(checks.Value(service.Identity(), "observed failed resource"),
            ResourceTrace::CloseMode::FailureAfterRelease);
        auto session = checks.Value(root->ObserveCleanup(), "observe without closing");
        checks.Require(trace->OpenCount() == 4 && !session.task().completed(),
            "obtaining cleanup observer triggered lifecycle close");
        return session;
        // The handle, child scope, root and borrowed adapter are released here.
        // Automatic disposal must preserve a terminal failure in owner-free state.
    }();
    auto status = checks.Value(observer.status(), "observe automatic disposal outcome");
    checks.Require(status.phase == pipelang::di::ScopePhase::Closed && status.pending == 0 &&
        status.attempted == 4 && status.failed == 1,
        "automatic disposal lost terminal cleanup progress/failure");
    checks.Require(observer.task().completed() && observer.task().result().is_failure(),
        "observer did not retain automatic disposal's failed terminal task");
    checks.Require(trace->OpenCount() == 0 && weakPort.expired(),
        "owner-free observer retained owner/graph or automatic disposal leaked resources");
    checks.CloseOrder(trace->Events(), {4, 3, 2, 1});
}

void ClosedResourceScopesDoNotConsumeLifetimeCapacityForever() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create resource churn root");
    for (int index = 0; index < 2200; ++index) {
        // Scope, handles, cache refs and successful cleanup records are released
        // every iteration; simultaneous ownership stays deliberately small.
        auto scope = checks.Value(root->CreateScope(Context(200 + index)), "create churn scope");
        auto service = checks.Value(scope->ResolveITransientResource(), "construct churn resource graph");
        checks.Success(scope->Close(), "close churn scope");
        checks.Require(service.Identity().is_failure(), "closed churn handle remained callable");
        checks.Require(trace->OpenCount() == 2, "closed scope retained an acquired resource");
    }
    checks.Success(root->Close(), "close churn root");
    checks.Require(trace->OpenCount() == 0, "resource churn leaked root resources");
}

void CleanupFailureProgressAndNoReplay() {
    for (const auto mode : {ResourceTrace::CloseMode::FailureAfterRelease,
                           ResourceTrace::CloseMode::CancelledAfterRelease,
                           ResourceTrace::CloseMode::ThrowBeforeRelease}) {
        auto trace = std::make_shared<ResourceTrace>();
        auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
        auto root = checks.Value(Startup::Create(port), "create cleanup error root");
        auto scope = checks.Value(root->CreateScope(Context(51)), "create cleanup error scope");
        auto transient = checks.Value(scope->ResolveITransientResource(), "construct cleanup error graph");
        trace->SetCloseMode(checks.Value(transient.Identity(), "cleanup error identity"), mode);
        auto closed = scope->Close();
        checks.Require(mode == ResourceTrace::CloseMode::CancelledAfterRelease ?
            closed.is_cancelled() : closed.is_failure(), "cleanup outcome category changed");
        checks.CloseOrder(trace->Events(), {4, 3});
        const auto progress = checks.Value(scope->CleanupStatus(), "cleanup progress");
        checks.Require(progress.pending == 0 && progress.attempted == 2,
            "cleanup error prevented monotonic progress to later resources");
        const auto count = trace->Events().size();
        auto repeated = scope->Close();
        checks.Require(repeated.outcome() == closed.outcome() && repeated.error() == closed.error(),
            "repeated close outcome changed");
        checks.Require(trace->Events().size() == count, "failed/cancelled/unknown cleanup replayed");
        const auto rootClosed = root->Close();
        checks.Require(rootClosed.outcome() == closed.outcome(),
            "root close hid its retained descendant cleanup outcome");
        checks.Require(trace->CloseAttempts(1) == 1 && trace->CloseAttempts(2) == 1,
            "retained descendant failure prevented independent root cleanup");
        const auto expectedOpen = mode == ResourceTrace::CloseMode::ThrowBeforeRelease ? 1U : 0U;
        checks.Require(trace->OpenCount() == expectedOpen,
            "cleanup status concealed an unreleased external effect");
        if (expectedOpen != 0) {
            trace->SetCloseMode(4, ResourceTrace::CloseMode::Success);
            checks.Success(trace->Close(4), "external test owner repairs deliberately unreleased resource");
        }
    }
}
}

int main() {
    try {
        NormalReverseOwnershipAndBorrowedInput();
        PartialFactoryAndFailedGraphUnwind();
        PreviousCacheSurvivesAndFailedServiceIsNotCached();
        CleanupFailureProgressAndNoReplay();
        ClosedResourceScopesDoNotConsumeLifetimeCapacityForever();
        OwnerFreeObserverSurvivesAutomaticDisposalAndReportsFailure();
        std::cout << "owned lifecycle integration assertions=" << checks.Count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
