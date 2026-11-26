// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
#ifdef PIPELANG_BOUNDED_PROJECT
using App = bounded_calls::source::Bounded::App;
using StepHost = bounded_calls::source::Bounded::StepHost;
#else
using bounded_calls::App;
using bounded_calls::StepHost;
#endif
using IntResult = pipelang::model::Result<std::int64_t, std::string>;
using IntTask = pipelang::model::Task<IntResult>;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

class Host final : public StepHost {
public:
    IntResult Step(pipelang::model::CancellationToken token) override {
        ++calls;
        if (token.is_cancelled())
            return IntResult::Cancelled("cancelled by fixture");
        return IntResult::Success(1);
    }

    IntResult Reenter() override {
        return reentry();
    }

    std::atomic<int> calls{0};
    std::function<IntResult()> reentry;
};

template<class Result>
void depthFailure(const Result& result) {
    require(result.is_failure() && result.error() == "source call depth limit exceeded",
        "source depth exhaustion was recovered, replaced, or unbounded");
}

template<class Task>
void finish(App& app, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        app.poll();
        require(std::chrono::steady_clock::now() < deadline, "bounded task failed to finish");
        std::this_thread::yield();
    }
}

auto depths(std::int64_t value, int count = 2) {
    auto values = pipelang::model::List<std::int64_t>::Empty();
    for (int index = 0; index < count; ++index)
        values.add(value);
    return values;
}

void synchronous(App& app) {
    require(app.NestedRunner().value() == 7, "nested runner callback target was lost");
    require(app.Visit().value() == 30, "ordinary visitor callback target was lost");
    require(app.Direct(30).value() == 1, "depth 32 was refused");
    const auto before = app.snapshot();
    depthFailure(app.Direct(31));
    require(app.snapshot() == before, "failed source chain published partial state");
    require(app.Siblings(30).value() == 2, "sibling ancestry was accumulated");
    require(app.Pure(30) == 1, "pure root/helper depth 32 was refused");
    bool pureFailed = false;
    try {
        (void)app.Pure(31);
    } catch (const pipelang::model::EvaluationLimitFailure&) {
        pureFailed = true;
    }
    require(pureFailed, "pure root/helper calls bypassed source ancestry");
#ifdef PIPELANG_BOUNDED_PROJECT
    require(app.Qualified(30).value() == 23, "qualified pure helper recursion failed");
    depthFailure(app.Qualified(31));
#endif
    require(app.Mutual(4).value() == 1, "mutual recursion failed");
    depthFailure(app.Mutual(20));
    require(app.Generic(30).value() == 17, "closed generic recursion failed");
    depthFailure(app.Generic(31));
    require(app.Virtual(4).value() == 2, "virtual/base recursive dispatch changed");
    depthFailure(app.Virtual(40));
    const auto work = app.Work();
    require(work.is_failure() && work.error() == "source evaluation budget exceeded",
        "shallow branching recursion escaped the shared work budget");
    depthFailure(app.CleanupDepth(false));
    require(app.Cleaned().value() == 31, "inline cleanup did not unwind entered object frames");
    depthFailure(app.CleanupDepth(true));
    require(app.Cleaned().value() == 61, "cleanup helper received extra depth or lost primary exhaustion");
    const auto cleanupWork = app.CleanupWork();
    require(cleanupWork.is_failure() && cleanupWork.error() == "primary fault",
        "cleanup exhaustion replaced the primary fault");
    require(app.Cleaned().value() > 1, "cleanup did not receive its independent work allowance");
    require(app.Construct(30).is_success() && app.ConstructedCount().value() == 31,
        "constructor depth did not count one activation per initializer");
    depthFailure(app.Construct(31));
    require(app.ConstructedCount().value() == 31, "rejected constructor ran its authored body");
    require(app.Construct(1).is_success(), "constructor failure leaked its savepoint");
    require(app.ParallelRoot(depths(30)).value() == 2, "root worker lost inherited ancestry");
    depthFailure(app.ParallelRoot(depths(31)));
    require(app.ParallelObject(depths(29)).value() == 2, "stateless object worker recursion failed");
    depthFailure(app.ParallelObject(depths(30)));
    const auto parallelWork = app.ParallelRoot(depths(1, 4096));
    require(parallelWork.is_failure() && parallelWork.error() == "source evaluation budget exceeded",
        "parallel siblings each received a fresh work allowance");
}

void asynchronous(App& app, Host& host) {
    for (const bool suspend : {false, true}) {
        auto local = app.LocalAsync(30, suspend, {});
        finish(app, local);
        require(local.result().is_success() && local.result().value() == 5, "local async depth 32 failed");
        auto localFailure = app.LocalAsync(31, suspend, {});
        finish(app, localFailure);
        depthFailure(localFailure.result());

        auto object = app.ObjectAsync(30, suspend, {});
        finish(app, object);
        require(object.result().is_success() && object.result().value() == 3, "interface async depth 32 failed");
        const int before = host.calls.load();
        auto objectFailure = app.ObjectAsync(31, suspend, {});
        finish(app, objectFailure);
        depthFailure(objectFailure.result());
        require(host.calls.load() - before == (suspend ? 31 : 0), "rejected async child ran host work");
    }
    auto first = app.AsyncSiblings(20, {});
    auto second = app.AsyncSiblings(20, {});
    finish(app, first);
    finish(app, second);
    require(first.result().value() == 6 && second.result().value() == 6,
        "interleaved task or sibling calls shared mutable depth");
    auto virtualSuccess = app.VirtualAsync(5, {});
    finish(app, virtualSuccess);
    require(virtualSuccess.result().value() == 3, "async virtual/base dispatch failed");
    auto virtualFailure = app.VirtualAsync(30, {});
    finish(app, virtualFailure);
    depthFailure(virtualFailure.result());
    auto work = app.AsyncWork();
    finish(app, work);
    require(work.result().is_failure() && work.result().error() == "source evaluation budget exceeded",
        "immediately awaited child reset its parent's work budget");
    auto cancelled = app.ObjectAsync(20, true, {});
    cancelled.cancel();
    finish(app, cancelled);
    require(cancelled.result().is_cancelled(), "async cancellation identity changed");
    require(app.pending() == 0, "completed chains retained task entries");
}

}

int main() {
    try {
        auto host = std::make_shared<Host>();
        App app(host);
        synchronous(app);
        asynchronous(app, *host);
        host->reentry = [&] { return app.Direct(0); };
        const auto reentry = app.HostReentry();
        require(reentry.is_failure() && reentry.error().find("reentry") != std::string::npos,
            "same-model host reentry escaped its existing refusal");
        bool wrongThread = false;
        std::thread other([&] {
            try {
                (void)app.Direct(0);
            } catch (const pipelang::model::ExecutionFailure&) {
                wrongThread = true;
            }
        });
        other.join();
        require(wrongThread, "wrong-owner source entry was admitted");
        std::optional<IntTask> retained;
        {
            App closing(host);
            retained = closing.ObjectAsync(20, true, {});
        }
        require(retained->completed() && retained->result().is_cancelled(), "teardown failed to release async chains");
        std::cout << "bounded source calls: " << assertions << " runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
