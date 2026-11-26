// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "IndependentTokenHost.hpp"

#include <array>
#include <iostream>

namespace {
using namespace token_test;
using Method = ReceiptTask (App::*)(CancellationToken, CancellationToken);
constexpr std::array<Method, 6> paths{
    &App::Direct, &App::Local, &App::ViaObject,
    &App::ViaInterface, &App::ViaVirtual, &App::ViaGeneric};

void precancelledArguments() {
    for (const auto method : paths) {
        for (const bool cancelBody : {false, true}) {
            auto host = std::make_shared<Host>();
            App app(host);
            CancellationSource source;
            source.cancel();
            const auto body = cancelBody ? source.token() : CancellationToken{};
            const auto cleanup = cancelBody ? CancellationToken{} : source.token();
            auto task = (app.*method)(body, cleanup);
            finish(app, task);
            const auto& result = receipt(task);
            require(result.bodyBefore == cancelBody && result.bodyAfter == cancelBody,
                "source call changed body token identity or cancellation state");
            require(result.cleanupBefore != cancelBody && result.cleanupAfter != cancelBody,
                "source call changed independent cleanup token identity");
            require(result.bodyOutcome == (cancelBody ? 2 : 1) && result.cleanupOutcome == (cancelBody ? 1 : 2),
                "an unrelated token cancelled the wrong concrete operation");
            require(host->phases() == std::vector<std::int64_t>({cancelBody ? 2 : 1}),
                "pre-cancelled explicit operand admitted work or poisoned the independent operation");
        }
    }
}

void cancellationDuringSuspension() {
    for (const auto method : paths) {
        for (const bool cancelBody : {false, true}) {
            auto host = std::make_shared<Host>();
            host->hold(1);
            App app(host);
            CancellationSource body;
            CancellationSource cleanup;
            auto task = (app.*method)(body.token(), cleanup.token());
            pumpUntil([&] { app.poll(); }, [&] { return host->entered(1); });
            require(!task.completed(), "multi-token source path did not really suspend");
            if (cancelBody)
                body.cancel();
            else
                cleanup.cancel();
            host->release(1);
            finish(app, task);
            const auto& result = receipt(task);
            require(!result.bodyBefore && !result.cleanupBefore, "live input tokens were contaminated on entry");
            require(result.bodyAfter == cancelBody && result.cleanupAfter != cancelBody,
                "in-flight cancellation crossed the source token boundary");
            require(result.bodyOutcome == (cancelBody ? 2 : 1) && result.cleanupOutcome == (cancelBody ? 1 : 2),
                "in-flight cancellation selected the wrong host operation");
        }
    }
}

void implicitTaskCancellation() {
    for (const auto method : paths) {
        for (const bool authoritative : {false, true}) {
            auto host = std::make_shared<Host>();
            host->hold(1, authoritative);
            App app(host);
            auto task = (app.*method)({}, {});
            pumpUntil([&] { app.poll(); }, [&] { return host->entered(1); });
            task.cancel();
            host->release(1);
            finish(app, task);
            const auto& result = receipt(task);
            require(!result.bodyBefore && !result.cleanupBefore && !result.bodyAfter && !result.cleanupAfter,
                "Task.cancel rewrote authored None tokens instead of implicit operation cancellation");
            require(result.bodyOutcome == (authoritative ? 1 : 2) && result.cleanupOutcome == 1,
                "implicit cancellation was lost, poisoned cleanup, or relabelled an authoritative host success");
            require(host->phases() == std::vector<std::int64_t>({1, 2}),
                "Task.cancel suppressed independent cleanup work");
        }
    }
}

void concreteSynchronizationAndContext() {
    CancellationSource cancelled;
    cancelled.cancel();
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto wait = app.Wait({}, cancelled.token());
        require(wait.is_success() && wait.value(), "synchronous Wait inherited an unrelated method token");
        require(app.LockValue(cancelled.token()).value() == 7,
            "lock inferred cancellation from an unused source parameter");
        auto values = List<std::int64_t>::Empty();
        values.add(2);
        values.add(3);
        require(app.ParallelValue(values, cancelled.token()).value() == 5,
            "parallel foreach inferred cancellation from an unused source parameter");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto wait = app.WaitAsync({}, cancelled.token());
        finish(app, wait);
        require(wait.result().is_success() && wait.result().value(),
            "WaitAsync inherited an unrelated method token");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        require(app.Wait(cancelled.token(), {}).is_cancelled(), "Wait ignored its concrete explicit token");
        auto wait = app.WaitAsync(cancelled.token(), {});
        finish(app, wait);
        require(wait.result().is_cancelled(), "WaitAsync ignored its concrete explicit token");
    }
    {
        ContextOwner authority;
        InvocationContextScope invocation(authority.capture().permit());
        auto host = std::make_shared<Host>();
        host->hold(1);
        App app(host);
        auto task = app.ViaInterface({}, {});
        pumpUntil([&] { app.poll(); }, [&] { return host->entered(1); });
        authority.invalidate();
        finish(app, task);
        require(task.result().is_cancelled(), "source token independence revived stale context authority");
        require(!host->entered(2), "stale cleanup authority admitted a new host operation");
    }
    {
        ContextOwner authority;
        InvocationContextScope invocation(authority.capture().permit());
        auto host = std::make_shared<Host>();
        App app(host);
        authority.invalidate();
        require(app.LockValue({}).is_cancelled(), "modern synchronous root lost implicit context cancellation");
    }
}
}

int main() {
    try {
        precancelledArguments();
        cancellationDuringSuspension();
        implicitTaskCancellation();
        concreteSynchronizationAndContext();
        std::cout << "independent tokens: " << token_test::assertions << " generated runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
