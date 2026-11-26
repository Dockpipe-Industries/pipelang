// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionFactory.hpp"
#include "IndependentTokenHost.hpp"

#include <iostream>

namespace {
using namespace token_test;
using Startup = independent_tokens::composition::Tokens::Startup;

template<class T>
T admitted(const Result<T>& value, const char* message) {
    require(value.is_success(), message);
    return value.value();
}

template<class TaskType>
void finish(const std::shared_ptr<Startup::Root>& root, const TaskType& task) {
    pumpUntil([&] { (void)admitted(root->Poll(), "DI poll failed"); }, [&] { return task.completed(); });
}

void authoredAndBoundaryTokens() {
    for (const bool cancelBody : {false, true}) {
        auto host = std::make_shared<Host>();
        auto root = admitted(Startup::Create(host), "create token root");
        auto scope = admitted(root->CreateScope(Stamp{0}), "create token scope");
        auto service = admitted(scope->ResolveITokenPath(), "resolve token path");
        CancellationSource cancelled;
        cancelled.cancel();
        const auto body = cancelBody ? cancelled.token() : CancellationToken{};
        const auto cleanup = cancelBody ? CancellationToken{} : cancelled.token();
        auto task = admitted(service.Run(body, cleanup), "authored cancellation incorrectly refused admission");
        finish(root, task);
        const auto& observed = receipt(task);
        require(observed.bodyBefore == cancelBody && observed.cleanupBefore != cancelBody,
            "composition SourceMethods rewrote authored tokens");
        require(observed.bodyOutcome == (cancelBody ? 2 : 1) && observed.cleanupOutcome == (cancelBody ? 1 : 2),
            "composition handle cancelled the wrong operation");
        const auto before = host->phases();
        require(service.Run({}, {}, cancelled.token()).is_cancelled(),
            "pre-cancelled separate boundary token did not refuse admission");
        require(host->phases() == before && admitted(root->Pending(), "pending query failed") == 0,
            "refused boundary admission entered source or retained a frame");
        require(root->Close().is_success(), "close token root");
    }
}

void taskAndBoundaryCancellation() {
    for (const bool cancelTask : {false, true}) {
        auto host = std::make_shared<Host>();
        host->hold(1);
        auto root = admitted(Startup::Create(host), "create cancelling token root");
        auto scope = admitted(root->CreateScope(Stamp{0}), "create cancelling token scope");
        auto service = admitted(scope->ResolveITokenPath(), "resolve cancelling token path");
        CancellationSource boundary;
        auto task = admitted(service.Run({}, {}, boundary.token()), "admit token boundary operation");
        pumpUntil([&] { (void)admitted(root->Poll(), "DI poll failed"); }, [&] { return host->entered(1); });
        if (cancelTask)
            task.cancel();
        else
            boundary.cancel();
        finish(root, task);
        const auto& observed = receipt(task);
        require(!observed.bodyAfter && !observed.cleanupAfter,
            "DI implicit cancellation mutated authored tokens");
        require(observed.bodyOutcome == 2 && observed.cleanupOutcome == 1,
            "DI task/boundary cancellation did not preserve independent cleanup");
        require(root->Close().is_success(), "close cancelling token root");
    }
}

void subtreeCancellation() {
    auto host = std::make_shared<Host>();
    host->hold(1);
    auto root = admitted(Startup::Create(host), "create group root");
    auto first = admitted(root->CreateScope(Stamp{0}), "create first group");
    auto second = admitted(root->CreateScope(Stamp{1}), "create second group");
    auto firstService = admitted(first->ResolveITokenPath(), "resolve first group");
    auto secondService = admitted(second->ResolveITokenPath(), "resolve second group");
    auto firstTask = admitted(firstService.Run({}, {}), "admit first group");
    CancellationSource unrelated;
    unrelated.cancel();
    auto secondTask = admitted(secondService.Wait({}, unrelated.token()), "admit independent group wait");
    pumpUntil([&] { (void)admitted(root->Poll(), "DI poll failed"); }, [&] { return host->entered(1); });
    require(!secondTask.completed(), "unused cleanup argument cancelled sibling group wait");
    auto closing = admitted(first->CloseAsync(), "begin first group close");
    finish(root, closing);
    require(closing.result().is_success(), "group close failed to drain source cleanup");
    const auto& observed = receipt(firstTask);
    require(!observed.bodyAfter && !observed.cleanupAfter && observed.bodyOutcome == 2 && observed.cleanupOutcome == 1,
        "group cancellation contaminated authored tokens or suppressed cleanup");
    require(host->phases() == std::vector<std::int64_t>({1, 2}), "group close did not execute awaited cleanup exactly once");
    require(!secondTask.completed(), "closing first group cancelled the independent waiter");
    require(secondService.ReleasePermit().is_success(), "release surviving group permit");
    finish(root, secondTask);
    require(secondTask.result().is_success(), "surviving group lost its result");
    require(firstService.Run({}, {}).is_failure(), "closed scope admitted another source operation");
    require(root->Close().is_success(), "close group root");
}
}

int main() {
    try {
        authoredAndBoundaryTokens();
        taskAndBoundaryCancellation();
        subtreeCancellation();
        std::cout << "independent tokens: " << token_test::assertions << " composition runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
