// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "AllocationProbe.hpp"
#include "manual/ManualScope.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Startup = di_fixture::composition::DiFixture::Bootstrap::Startup;
using Stamp = di_fixture::source::DiFixture::Contracts::ScopeStamp;
std::int64_t checksum = 0;

template<class Result>
void Require(const Result& outcome) {
    if (!outcome.is_success())
        throw std::runtime_error("benchmark correctness precondition failed");
}

template<class Function>
auto Measure(const char* lane, int sample, const char* phase, int operations, Function function) {
    di_tests::BeginAllocationProbe();
    const auto start = std::chrono::steady_clock::now();
    auto outcome = function();
    const auto end = std::chrono::steady_clock::now();
    const auto allocations = di_tests::EndAllocationProbe();
    Require(outcome);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "{\"lane\":\"" << lane << "\",\"sample\":" << sample
              << ",\"phase\":\"" << phase << "\",\"operations\":" << operations
              << ",\"wall_ns\":" << nanoseconds << ",\"allocations\":" << allocations.calls
              << ",\"requested_bytes\":" << allocations.requestedBytes << "}\n";
    return outcome;
}

// The manual lane must preserve rollback, cancellation and owner guards before
// its timing samples are accepted; these checks are deliberately untimed.
template<class Root>
void Preflight() {
    auto policy = std::make_shared<di_tests::ConstructionPolicy>();
    auto created = Root::Create(policy);
    Require(created);
    auto root = created.value();
    auto sequence = root->ResolveIIdentitySequence();
    Require(sequence);
    policy->selectedKind = "faulty";
    policy->decision = di_tests::ConstructionPolicy::Decision::Failure;
    if (!root->ResolveIFaultyService().is_failure())
        throw std::runtime_error("failed constructor published a service");
    auto count = sequence.value().Count();
    Require(count);
    if (count.value() != 0)
        throw std::runtime_error("failed construction did not roll back source mutations");
    policy->decision = di_tests::ConstructionPolicy::Decision::ThrowCancellation;
    if (!root->ResolveIFaultyService().is_cancelled())
        throw std::runtime_error("constructor cancellation lost its outcome");
    count = sequence.value().Count();
    Require(count);
    if (count.value() != 0)
        throw std::runtime_error("cancelled construction did not roll back");
    policy->decision = di_tests::ConstructionPolicy::Decision::Success;
    auto recovered = root->ResolveIFaultyService();
    Require(recovered);
    auto repeated = root->ResolveIFaultyService();
    Require(repeated);
    if (recovered.value().Identity().value() != repeated.value().Identity().value() || policy->ReadsOf("faulty") != 3)
        throw std::runtime_error("manual/generated cache publication diverged");
    bool reentryRejected = false;
    policy->onRead = [&](const std::string&, std::int64_t) {
        reentryRejected = root->Close().is_failure();
    };
    Stamp stamp;
    stamp.identity = 8;
    auto scope = root->CreateScope(stamp);
    Require(scope);
    auto useCase = scope.value()->ResolveIUseCase();
    Require(useCase);
    if (!reentryRejected)
        throw std::runtime_error("close callback mutated an active owner");
    policy->onRead = {};
    bool wrongThreadRejected = false;
    std::thread worker([&] { wrongThreadRejected = root->ResolveIRootService().is_failure(); });
    worker.join();
    if (!wrongThreadRejected)
        throw std::runtime_error("wrong-owner resolution was admitted");
    Require(scope.value()->Close());
    Require(root->Close());
}

void DirectBridgeReentry() {
    namespace Contracts = di_fixture::source::DiFixture::Contracts;
    namespace Roots = di_fixture::source::DiFixture::Root;
    auto owner = pipelang::di::ExecutionOwner::Create();
    Require(owner);
    auto context = owner.value()->rootExecution();
    Require(context);
    auto outcome = context.value()->invoke<int>({}, [&](pipelang::model::ObjectExecution& execution) {
        auto sequence = Contracts::IIdentitySequence(
            di_fixture::construction::Construct<Roots::IdentitySequence>(execution));
        bool constructorRejected = false;
        bool methodRejected = false;
        execution.scope.invokeSynchronousHost([&] {
            try {
                di_fixture::construction::Construct<Roots::IdentitySequence>(execution);
            } catch (const pipelang::model::ExecutionFailure&) {
                constructorRejected = true;
            }
            try {
                di_fixture::construction::SourceMethods<Contracts::IIdentitySequence>::Count(execution, sequence);
            } catch (const pipelang::model::ExecutionFailure&) {
                methodRejected = true;
            }
            return 1;
        });
        if (!constructorRejected || !methodRejected)
            throw std::runtime_error("direct source bridge admitted native host reentry");
        return 1;
    });
    Require(outcome);
    Require(owner.value()->close());
}

template<class Root>
void Run(const char* lane, int sample) {
    auto policy = std::make_shared<di_tests::ConstructionPolicy>();
    policy->trace = false;
    auto root = Measure(lane, sample, "root_startup", 1, [&] { return Root::Create(policy); }).value();
    Stamp stamp;
    stamp.identity = 7;
    stamp.tenant = "benchmark";
    auto scope = Measure(lane, sample, "scope_creation", 1, [&] { return root->CreateScope(stamp); }).value();
    auto singleton = Measure(lane, sample, "first_singleton", 1, [&] { return root->ResolveIRootService(); }).value();
    Measure(lane, sample, "cached_singleton", 1000, [&] {
        auto outcome = root->ResolveIRootService();
        for (int index = 1; index < 1000; ++index) {
            Require(outcome);
            outcome = root->ResolveIRootService();
        }
        return outcome;
    });
    auto request = Measure(lane, sample, "first_scoped", 1, [&] { return scope->ResolveIRequestService(); }).value();
    Measure(lane, sample, "cached_scoped", 1000, [&] {
        auto outcome = scope->ResolveIRequestService();
        for (int index = 1; index < 1000; ++index) {
            Require(outcome);
            outcome = scope->ResolveIRequestService();
        }
        return outcome;
    });
    Measure(lane, sample, "guarded_method", 1000, [&] {
        auto outcome = request.Identity();
        for (int index = 1; index < 1000; ++index) {
            Require(outcome);
            checksum += outcome.value();
            outcome = request.Identity();
        }
        return outcome;
    });
    Measure(lane, sample, "transient", 64, [&] {
        auto outcome = scope->ResolveIUseCase();
        for (int index = 1; index < 64; ++index) {
            Require(outcome);
            outcome = scope->ResolveIUseCase();
        }
        return outcome;
    });
    auto sequence = root->ResolveIIdentitySequence();
    Require(sequence);
    auto count = sequence.value().Count();
    Require(count);
    if (count.value() != 67)
        throw std::runtime_error("manual and generated construction count diverged");
    Measure(lane, sample, "scope_close", 1, [&] { return scope->Close(); });
    if (request.Identity().is_success())
        throw std::runtime_error("closed scoped handle remained callable");
    Require(singleton.Identity());
    Measure(lane, sample, "root_close", 1, [&] { return root->Close(); });
    if (singleton.Identity().is_success())
        throw std::runtime_error("closed root handle remained callable");
}
}

int main() {
    try {
        Preflight<Startup>();
        Preflight<di_tests::manual::ManualRoot>();
        DirectBridgeReentry();
        // Untimed warmup exercises identical construction and lifetime assertions.
        Run<Startup>("generated_warmup", -1);
        Run<di_tests::manual::ManualRoot>("manual_warmup", -1);
        for (int sample = 0; sample < 7; ++sample) {
            if (sample % 2 == 0) {
                Run<Startup>("generated", sample);
                Run<di_tests::manual::ManualRoot>("manual", sample);
            } else {
                Run<di_tests::manual::ManualRoot>("manual", sample);
                Run<Startup>("generated", sample);
            }
        }
        std::cerr << "matched ownership benchmark passed; checksum=" << checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
