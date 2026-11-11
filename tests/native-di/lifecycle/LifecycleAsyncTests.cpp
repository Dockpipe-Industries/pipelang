// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "LifecycleAssertions.hpp"
#include "ResourcePortAdapter.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {
using Startup = lifecycle_fixture::composition::DiLifecycle::Bootstrap::Startup;
using Stamp = lifecycle_fixture::source::DiLifecycle::Contracts::ScopeStamp;
using Task = pipelang::model::Task<pipelang::model::Result<std::int64_t>>;
using lifecycle_test::ResourceTrace;
lifecycle_test::LifecycleAssertions checks;

Stamp Context(std::int64_t identity) {
    Stamp stamp;
    stamp.identity = identity;
    return stamp;
}

template<class TaskType>
void Finish(const std::shared_ptr<Startup::Root>& root, const TaskType& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        checks.Value(root->Poll(), "owner poll");
        checks.Require(std::chrono::steady_clock::now() < deadline, "scoped async work failed to drain");
        std::this_thread::yield();
    }
}

void RequireOrder(const std::vector<lifecycle_test::ResourceEvent>& events,
    const std::string& first, const std::string& second, std::int64_t id) {
    std::size_t firstIndex = events.size();
    std::size_t secondIndex = events.size();
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index].resource != id)
            continue;
        if (events[index].operation == first)
            firstIndex = index;
        if (events[index].operation == second)
            secondIndex = index;
    }
    checks.Require(firstIndex < secondIndex && secondIndex < events.size(),
        first + " must precede " + second + " for resource " + std::to_string(id));
}

void LocalTasksHaveIndependentScopeCancellation() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create local async root");
    auto firstScope = checks.Value(root->CreateScope(Context(71)), "create first async scope");
    auto secondScope = checks.Value(root->CreateScope(Context(72)), "create second async scope");
    auto first = checks.Value(firstScope->ResolveIScopedResource(), "first async service");
    auto second = checks.Value(secondScope->ResolveIScopedResource(), "second async service");
    auto firstTask = checks.Value(first.LocalWork(), "admit first local work");
    auto secondTask = checks.Value(second.LocalWork(), "admit second local work");
    checks.Require(!firstTask.completed() && !secondTask.completed(), "local wait did not suspend");
    auto closing = checks.Value(firstScope->CloseAsync(), "begin first scope async close");
    // Observer cancellation cannot reopen lifecycle or suppress independent
    // resource cleanup; the terminal close outcome remains authoritative.
    closing.cancel();
    Finish(root, closing);
    checks.Success(closing.result(), "first async close result");
    checks.Require(firstTask.completed() && firstTask.result().is_cancelled(),
        "scope close did not cancel/drain its own source wait");
    checks.Require(!secondTask.completed(), "closing one scope cancelled another scope's task");
    checks.Require(second.Identity().is_success(), "independent scope became unavailable");
    checks.Success(second.ReleaseLocal(), "release surviving scope wait");
    Finish(root, secondTask);
    checks.Require(secondTask.result().is_success(), "surviving scope lost its successful task outcome");
    RequireOrder(trace->Events(), "local-work-finally", "close-attempt", 3);
    checks.Success(secondScope->Close(), "close surviving scope");
    checks.Success(root->Close(), "close local async root");
    checks.Require(trace->OpenCount() == 0, "local async close leaked resources");
}

void PendingSourceTaskAndObserverDoNotRetainDroppedOwner() {
    auto trace = std::make_shared<ResourceTrace>();
    std::weak_ptr<lifecycle_test::ResourcePortAdapter> weakPort;
    auto retained = [&] {
        auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
        weakPort = port;
        auto root = checks.Value(Startup::Create(port), "create pending-drop root");
        auto scope = checks.Value(root->CreateScope(Context(74)), "create pending-drop scope");
        auto service = checks.Value(scope->ResolveIScopedResource(), "pending-drop service");
        auto task = checks.Value(service.LocalWork(), "admit pending-drop source task");
        checks.Require(!task.completed(), "pending-drop source task did not suspend");
        auto observer = checks.Value(root->ObserveCleanup(), "observe pending-drop cleanup");
        return std::make_pair(observer, task);
        // No explicit Close/Poll: every facade and source handle drops here.
        // The retained task and observer must not keep ExecutionOwner alive.
    }();
    const auto status = checks.Value(retained.first.status(), "pending-drop cleanup status");
    checks.Require(status.phase == pipelang::di::ScopePhase::Closed && status.pending == 0 &&
        status.attempted == 3 && status.failed == 0, "final facade drop did not complete owned disposal");
    checks.Require(retained.second.completed() && retained.second.result().is_cancelled(),
        "final facade drop failed to cancel/drain actual source task");
    checks.Require(retained.first.task().completed() && retained.first.task().result().is_success(),
        "owner-free observer lost final-drop cleanup completion");
    RequireOrder(trace->Events(), "local-work-finally", "close-attempt", 3);
    checks.Require(trace->OpenCount() == 0 && weakPort.expired(),
        "retained task/observer kept execution owner alive or leaked owned resources");
}

void PrecancelledAuthoredAndBoundaryTokensRefuseBeforeSourceWork() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create precancelled async root");
    auto scope = checks.Value(root->CreateScope(Context(75)), "create precancelled async scope");
    auto service = checks.Value(scope->ResolveIScopedResource(), "precancelled async service");
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    checks.Require(service.Work(cancellation.token()).is_cancelled(),
        "pre-cancelled authored token admitted source work");
    checks.Require(service.Work(pipelang::model::CancellationToken{}, cancellation.token()).is_cancelled(),
        "pre-cancelled boundary token admitted source work");
    checks.Require(checks.Value(root->Pending(), "precancelled pending count") == 0,
        "pre-cancelled async admission created a retained task");
    for (const auto& event : trace->Events()) {
        checks.Require(event.operation != "wait-enter" && event.operation != "work-finally",
            "pre-cancelled admission entered source/host work");
    }
    checks.Success(root->Close(), "close precancelled async root");
}

void HostWorkCancelsAndDrainsBeforeResourceClose() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create host async root");
    auto scope = checks.Value(root->CreateScope(Context(81)), "create host async scope");
    auto service = checks.Value(scope->ResolveIScopedResource(), "host async service");
    auto task = checks.Value(service.Work(pipelang::model::CancellationToken{}), "admit host work");
    checks.Require(trace->WaitEntered(3), "host worker never entered its bounded wait");
    auto closing = checks.Value(scope->CloseAsync(), "begin host scope close");
    Finish(root, closing);
    checks.Success(closing.result(), "host scope close result");
    checks.Require(task.completed() && task.result().is_cancelled(), "host task did not drain as Cancelled");
    RequireOrder(trace->Events(), "wait-cancelled", "work-finally", 3);
    RequireOrder(trace->Events(), "work-finally", "close-attempt", 3);
    checks.Require(service.Identity().is_failure(), "closed async handle remained callable");
    checks.Success(root->Close(), "close host async root");
    checks.Require(trace->OpenCount() == 0, "host async close leaked resources");
}

void AuthoritativeHostSuccessDrainsThroughSourceFinallyBeforeRootClose() {
    auto trace = std::make_shared<ResourceTrace>();
    auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
    auto root = checks.Value(Startup::Create(port), "create authoritative async root");
    auto scope = checks.Value(root->CreateScope(Context(82)), "create authoritative async scope");
    auto service = checks.Value(scope->ResolveIScopedResource(), "authoritative async service");
    auto task = checks.Value(service.Work(pipelang::model::CancellationToken{}), "admit authoritative host work");
    checks.Require(trace->WaitEntered(3), "authoritative host worker never entered");
    trace->CompleteWait(3);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool committed = false;
    while (!committed && std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : trace->Events()) {
            if (event.operation == "wait-success" && event.resource == 3)
                committed = true;
        }
        std::this_thread::yield();
    }
    checks.Require(committed, "host success never reached its authoritative outcome");
    // No owner Poll before close: drain must preserve the host result and run
    // authored source finally before disposing the receiver and dependencies.
    auto closing = checks.Value(root->CloseAsync(), "begin authoritative root close");
    Finish(root, closing);
    checks.Success(closing.result(), "authoritative root close result");
    if (!task.completed() || !task.result().is_success()) {
        std::cerr << "authoritative task completed=" << task.completed();
        if (task.completed())
            std::cerr << " outcome=" << static_cast<int>(task.result().outcome())
                      << " error=" << task.result().error();
        std::cerr << '\n';
        trace->Dump(std::cerr);
    }
    checks.Require(task.completed() && task.result().is_success() && task.result().value() == 3,
        "root close relabelled an authoritative host success as cancellation");
    RequireOrder(trace->Events(), "wait-success", "work-finally", 3);
    RequireOrder(trace->Events(), "work-finally", "close-attempt", 3);
    checks.Require(trace->OpenCount() == 0, "authoritative close leaked resources");
}

void IndependentOwnersRunOnTheirOwnThreads() {
    std::array<bool, 2> success{};
    std::array<std::string, 2> failures;
    std::array<std::thread, 2> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index] {
            try {
                auto trace = std::make_shared<ResourceTrace>();
                auto port = std::make_shared<lifecycle_test::ResourcePortAdapter>(trace);
                auto created = Startup::Create(port);
                if (!created.is_success())
                    throw std::runtime_error("independent root creation failed");
                auto root = created.value();
                auto createdScope = root->CreateScope(Context(100 + index));
                if (!createdScope.is_success())
                    throw std::runtime_error("independent scope creation failed");
                auto scope = createdScope.value();
                auto service = scope->ResolveITransientResource();
                if (!service.is_success() || !service.value().Identity().is_success())
                    throw std::runtime_error("independent resource resolution failed");
                if (!root->Close().is_success() || trace->OpenCount() != 0)
                    throw std::runtime_error("independent owner disposal failed");
                success[index] = true;
            } catch (const std::exception& error) {
                failures[index] = error.what();
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    for (std::size_t index = 0; index < threads.size(); ++index)
        checks.Require(success[index], "independent owner thread: " + failures[index]);
}
}

int main() {
    try {
        LocalTasksHaveIndependentScopeCancellation();
        PendingSourceTaskAndObserverDoNotRetainDroppedOwner();
        PrecancelledAuthoredAndBoundaryTokensRefuseBeforeSourceWork();
#if !defined(__EMSCRIPTEN__) && !defined(__wasm__)
        HostWorkCancelsAndDrainsBeforeResourceClose();
        AuthoritativeHostSuccessDrainsThroughSourceFinallyBeforeRootClose();
        IndependentOwnersRunOnTheirOwnThreads();
#endif
        std::cout << "owned scoped async assertions=" << checks.Count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
