// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../AllocationProbe.hpp"
#include "ExpectedResourceTrace.hpp"
#include "LifecycleAssertions.hpp"
#include "manual/Scope.hpp"
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using Startup = lifecycle_fixture::composition::DiLifecycle::Bootstrap::Startup;
using Stamp = lifecycle_fixture::source::DiLifecycle::Contracts::ScopeStamp;
lifecycle_test::LifecycleAssertions checks;

Stamp Context(std::int64_t identity, std::int64_t failure = 0) {
    Stamp stamp;
    stamp.identity = identity;
    stamp.failureMode = failure;
    return stamp;
}

template<class Root, class Task>
void Finish(const std::shared_ptr<Root>& root, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        const auto polled = root->Poll();
        if (!polled.is_success())
            throw std::runtime_error(polled.error());
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("benchmark close failed to drain");
        std::this_thread::yield();
    }
}

template<class Provider>
void Preflight() {
    auto trace = std::make_shared<lifecycle_test::ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    checks.Value(trace->Open("borrowed", 0), "manual parity borrowed sentinel");
    auto root = checks.Value(Provider::Create(port), "manual parity root");
    auto firstScope = checks.Value(root->CreateScope(Context(11)), "manual parity first scope");
    checks.Value(firstScope->ResolveITransientResource(), "manual parity first graph");
    auto secondScope = checks.Value(root->CreateScope(Context(22)), "manual parity second scope");
    checks.Value(secondScope->ResolveITransientResource(), "manual parity second graph");
    checks.Success(firstScope->Close(), "manual parity first close");
    checks.Success(root->Close(), "manual parity recursive root close");
    checks.Trace(trace->Events(), lifecycle_test::ExpectedResourceTrace::NormalTwoScopes());
    checks.Require(trace->OpenCount() == 1, "manual parity disposed borrowed sentinel");
    checks.Success(trace->Close(1), "manual parity external sentinel close");

    trace = std::make_shared<lifecycle_test::ResourceTrace>();
    port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    root = checks.Value(Provider::Create(port), "manual parity failure root");
    auto cached = checks.Value(root->ResolveIRootResource(), "manual parity cached root");
    auto failedScope = checks.Value(root->CreateScope(Context(31, 1)), "manual parity failure scope");
    checks.Require(failedScope->ResolveIScopedResource().is_failure(), "manual parity failed factory succeeded");
    checks.Require(failedScope->ResolveIScopedResource().is_failure(), "manual parity failed factory was cached");
    checks.Require(cached.Identity().is_success() && trace->CloseAttempts(1) == 0 && trace->CloseAttempts(2) == 0,
        "manual parity failed attempt destroyed prior cache");
    checks.Success(root->Close(), "manual parity failure root close");
    checks.Require(trace->OpenCount() == 0, "manual parity factory finally leaked");
}

template<class Function>
auto Measure(const char* lane, int sample, const char* phase, int operations, Function function) {
    di_tests::BeginAllocationProbe();
    const auto start = std::chrono::steady_clock::now();
    auto outcome = function();
    const auto end = std::chrono::steady_clock::now();
    const auto allocations = di_tests::EndAllocationProbe();
    checks.Require(outcome.is_success(), std::string("measured lifecycle phase failed: ") + phase);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "{\"lane\":\"" << lane << "\",\"sample\":" << sample
              << ",\"phase\":\"" << phase << "\",\"operations\":" << operations
              << ",\"wall_ns\":" << nanoseconds << ",\"allocations\":" << allocations.calls
              << ",\"requested_bytes\":" << allocations.requestedBytes << "}\n";
    return outcome;
}

template<class Provider>
void Run(const char* lane, int sample) {
    auto trace = std::make_shared<lifecycle_test::ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = Measure(lane, sample, "root_startup", 1, [&] { return Provider::Create(port); }).value();
    auto scope = Measure(lane, sample, "scope_creation", 1, [&] { return root->CreateScope(Context(101)); }).value();
    auto singleton = Measure(lane, sample, "first_owned_singleton", 1, [&] {
        return root->ResolveIRootResource();
    }).value();
    auto service = Measure(lane, sample, "first_owned_scoped", 1, [&] {
        return scope->ResolveIScopedResource();
    }).value();
    Measure(lane, sample, "cached_owned_singleton", 1000, [&] {
        auto outcome = root->ResolveIRootResource();
        for (int index = 1; index < 1000; ++index) {
            if (!outcome.is_success())
                throw std::runtime_error("cached singleton resolution failed");
            outcome = root->ResolveIRootResource();
        }
        return outcome;
    });
    Measure(lane, sample, "cached_owned_scoped", 1000, [&] {
        auto outcome = scope->ResolveIScopedResource();
        for (int index = 1; index < 1000; ++index) {
            if (!outcome.is_success())
                throw std::runtime_error("cached scoped resolution failed");
            outcome = scope->ResolveIScopedResource();
        }
        return outcome;
    });
    Measure(lane, sample, "owned_transient", 16, [&] {
        auto outcome = scope->ResolveITransientResource();
        for (int index = 1; index < 16; ++index) {
            if (!outcome.is_success())
                throw std::runtime_error("transient resource construction failed");
            outcome = scope->ResolveITransientResource();
        }
        return outcome;
    });
    Measure(lane, sample, "scope_dispose_17_resources", 17, [&] { return scope->Close(); });
    checks.Require(trace->OpenCount() == 2 && service.Identity().is_failure() && singleton.Identity().is_success(),
        "measured scope disposal changed lifetime ownership");
    auto asyncScope = checks.Value(root->CreateScope(Context(102)), "create measured async scope");
    auto asyncService = checks.Value(asyncScope->ResolveIScopedResource(), "construct measured async service");
    auto task = Measure(lane, sample, "local_async_admission", 1, [&] { return asyncService.LocalWork(); }).value();
    Measure(lane, sample, "cancel_drain_dispose_one_resource", 1, [&] {
        auto closing = asyncScope->CloseAsync();
        if (!closing.is_success())
            throw std::runtime_error("measured async close admission failed");
        Finish(root, closing.value());
        return closing.value().result();
    });
    checks.Require(task.completed() && task.result().is_cancelled() && trace->OpenCount() == 2,
        "measured async cancellation/drain/disposal parity failed");
    Measure(lane, sample, "root_dispose_two_resources", 2, [&] { return root->Close(); });
    checks.Require(trace->OpenCount() == 0, "measured root disposal leaked resources");
}
}

int main() {
    try {
        Preflight<Startup>();
        Preflight<lifecycle_test::manual::Root>();
        Run<Startup>("generated_warmup", -1);
        Run<lifecycle_test::manual::Root>("manual_warmup", -1);
        for (int sample = 0; sample < 7; ++sample) {
            if (sample % 2 == 0) {
                Run<Startup>("generated", sample);
                Run<lifecycle_test::manual::Root>("manual", sample);
            } else {
                Run<lifecycle_test::manual::Root>("manual", sample);
                Run<Startup>("generated", sample);
            }
        }
        std::cerr << "owned lifecycle manual parity assertions=" << checks.Count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
