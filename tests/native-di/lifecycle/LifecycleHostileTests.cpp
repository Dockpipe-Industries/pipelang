// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "LifecycleAssertions.hpp"
#include "ResourcePortAdapter.hpp"
#include <iostream>
#include <set>
#include <thread>

namespace {
using Startup = lifecycle_fixture::composition::DiLifecycle::Bootstrap::Startup;
using Stamp = lifecycle_fixture::source::DiLifecycle::Contracts::ScopeStamp;
using lifecycle_test::ResourceTrace;
lifecycle_test::LifecycleAssertions checks;

Stamp Context(std::int64_t mode = 0, std::int64_t work = 0) {
    Stamp stamp;
    stamp.identity = 61;
    stamp.failureMode = mode;
    stamp.cleanupWork = work;
    return stamp;
}

void CancelledPartialAcquisitionUsesRealNoncancelledFinallyToken() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create cancelled factory root");
    auto cached = checks.Value(root->ResolveIRootResource(), "prime root cache");
    auto scope = checks.Value(root->CreateScope(Context()), "create cancelled factory scope");
    pipelang::model::CancellationSource cancellation;
    trace->onOpen = [&](std::int64_t, const std::string& kind, std::int64_t) {
        if (kind == "scoped")
            cancellation.cancel();
    };
    checks.Require(scope->ResolveIScopedResource(cancellation.token()).is_cancelled(),
        "cancelled partial acquisition did not remain Cancelled");
    if (trace->CloseAttempts(3) != 1 || trace->OpenCount() != 2) {
        std::cerr << "partial factory after cancellation: closeAttempts=" << trace->CloseAttempts(3)
                  << " openResources=" << trace->OpenCount() << '\n';
        trace->Dump(std::cerr);
    }
    checks.Require(trace->CloseAttempts(3) == 1 && trace->OpenCount() == 2,
        "factory finally failed to release acquired resource with CancellationToken.None");
    checks.Require(cached.Identity().is_success(), "cancelled factory discarded prior root cache");
    trace->onOpen = {};
    checks.Success(root->Close(), "close cancelled factory root");
}

void CancellationAfterFactoryTransferUnwindsLiveOwnedGraph() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create transferred factory root");
    auto cached = checks.Value(root->ResolveIRootResource(), "prime transferred factory root");
    auto scope = checks.Value(root->CreateScope(Context(4)), "create transferred factory scope");
    pipelang::model::CancellationSource cancellation;
    trace->onNote = [&](const std::string& operation, std::int64_t, std::int64_t) {
        if (operation == "factory-transferred")
            cancellation.cancel();
    };
    checks.Require(scope->ResolveIScopedResource(cancellation.token()).is_cancelled(),
        "last factory callback cancellation published its service");
    bool transferred = false;
    bool typedCleanup = false;
    for (const auto& event : trace->Events()) {
        if (event.operation == "factory-transferred" && event.resource == 3)
            transferred = true;
        if (event.operation == "cleanup-enter" && event.resource == 3 && event.dependency == 2)
            typedCleanup = transferred;
    }
    checks.Require(typedCleanup && trace->CloseAttempts(3) == 1,
        "successful factory transfer was not adopted/unwound before graph discard");
    checks.Require(trace->CloseAttempts(1) == 0 && trace->CloseAttempts(2) == 0 &&
        cached.Identity().is_success(), "post-transfer cancellation unwound previously cached resources");
    trace->onNote = {};
    checks.Success(root->Close(), "close post-transfer cancellation root");
    checks.Require(trace->OpenCount() == 0, "post-transfer cancellation leaked resource");
}

void CleanupExhaustionAdvancesWithoutClaimingReleaseOrReplay() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create exhausted cleanup root");
    auto scope = checks.Value(root->CreateScope(Context(0, 1000000000)), "create exhausted cleanup scope");
    checks.Value(scope->ResolveITransientResource(), "construct exhausted cleanup graph");
    auto closed = root->Close();
    checks.Require(closed.is_failure(), "cleanup budget exhaustion did not report Failure");
    auto status = checks.Value(root->CleanupStatus(), "exhausted cleanup status");
    // A pre-claim reserve exhaustion can defer unattempted records to a later
    // bounded close call. The already claimed exhausted callback is terminal.
    for (int pass = 0; status.pending != 0 && pass < 8; ++pass) {
        closed = root->Close();
        status = checks.Value(root->CleanupStatus(), "resumed cleanup status");
    }
    checks.Require(status.pending == 0 && status.unknown != 0,
        "bounded close progress stalled or concealed unknown effects");
    std::size_t exhaustedEntries = 0;
    for (const auto& event : trace->Events()) {
        if (event.operation == "cleanup-enter" && event.resource == 3)
            ++exhaustedEntries;
    }
    checks.Require(exhaustedEntries == 1, "claimed exhausted cleanup replayed during bounded progress");
    checks.Require(trace->CloseAttempts(4) == 1 && trace->CloseAttempts(3) == 0,
        "exhausted source cleanup falsely reported releasing its resource");
    const auto count = trace->Events().size();
    auto repeated = root->Close();
    checks.Require(repeated.outcome() == closed.outcome() && repeated.error() == closed.error(),
        "exhausted close outcome was not stable");
    checks.Require(trace->Events().size() == count, "exhausted source cleanup replayed");
    checks.Require(trace->OpenCount() != 0, "test did not preserve the deliberately unknown external resource");

    // Explicit external recovery is test teardown, not an automatic DI retry.
    std::set<std::int64_t> unreleased;
    for (const auto& event : trace->Events()) {
        if (event.operation == "open")
            unreleased.insert(event.resource);
        if (event.operation == "close-effect")
            unreleased.erase(event.resource);
    }
    for (const auto resource : unreleased)
        checks.Success(trace->Close(resource), "external teardown repairs known synthetic resource");
}

void WrongOwnerAndHostReentryRefuseBeforeMutation() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create guarded resource root");
    auto scope = checks.Value(root->CreateScope(Context()), "create guarded resource scope");
    bool constructorReentry = false;
    trace->onOpen = [&](std::int64_t, const std::string& kind, std::int64_t) {
        if (kind == "scoped")
            constructorReentry = root->Close().is_failure();
    };
    auto service = checks.Value(scope->ResolveIScopedResource(), "construct guarded resource");
    checks.Require(constructorReentry && service.Identity().is_success(),
        "constructor callback close mutated the live owner");
    trace->onOpen = {};
    bool foreignResolve = false;
    bool foreignInvoke = false;
    bool foreignClose = false;
    std::thread worker([&] {
        foreignResolve = scope->ResolveIScopedResource().is_failure();
        foreignInvoke = service.Identity().is_failure();
        foreignClose = root->Close().is_failure();
    });
    worker.join();
    checks.Require(foreignResolve && foreignInvoke && foreignClose,
        "foreign thread admitted resource resolution/invocation/disposal");
    std::size_t refusedCloseCallbacks = 0;
    trace->onClose = [&](std::int64_t) {
        if (root->Close().is_failure() && scope->Close().is_failure())
            ++refusedCloseCallbacks;
    };
    checks.Success(root->Close(), "outer guarded close");
    checks.Require(refusedCloseCallbacks == 3 && trace->OpenCount() == 0,
        "cleanup callback reentry changed exact disposal progress");
    trace->onClose = {};
}
}

int main() {
    try {
        CancelledPartialAcquisitionUsesRealNoncancelledFinallyToken();
        CancellationAfterFactoryTransferUnwindsLiveOwnedGraph();
        CleanupExhaustionAdvancesWithoutClaimingReleaseOrReplay();
#if !defined(__EMSCRIPTEN__) && !defined(__wasm__)
        WrongOwnerAndHostReentryRefuseBeforeMutation();
#endif
        std::cout << "owned resource hostile assertions=" << checks.Count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
