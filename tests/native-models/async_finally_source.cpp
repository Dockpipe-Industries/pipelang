// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
#ifdef PIPELANG_ASYNC_FINALLY_PROJECT
using App = async_finally::source::Cleanup::App;
using CleanupHost = async_finally::source::Cleanup::CleanupHost;
using FacetProbe = async_finally::source::Cleanup::FacetProbe;
#else
using async_finally::App;
using async_finally::CleanupHost;
#endif
using namespace pipelang::model;
using IntResult = Result<std::int64_t>;
using IntTask = Task<IntResult>;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

class Host final : public CleanupHost {
public:
    IntResult Pause(std::int64_t phase, CancellationToken token) override {
        std::unique_lock lock(mutex_);
        phases_.push_back(phase);
        changed_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (held_[phase] && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return IntResult::Failure("fixture host gate deadline");
            changed_.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (token.is_cancelled())
            return IntResult::Cancelled("fixture operation cancelled");
        return IntResult::Success(phase);
    }
    Result<void> Mark(std::int64_t phase) override {
        if (onMark)
            onMark();
        std::lock_guard lock(mutex_);
        marks_.push_back(phase);
        return Result<void>::Success();
    }
    void hold(std::int64_t phase) {
        std::lock_guard lock(mutex_);
        held_[phase] = true;
    }
    void release(std::int64_t phase) {
        std::lock_guard lock(mutex_);
        held_[phase] = false;
        changed_.notify_all();
    }
    bool entered(std::int64_t phase) const {
        const auto values = phases();
        return std::find(values.begin(), values.end(), phase) != values.end();
    }
    std::vector<std::int64_t> phases() const {
        std::lock_guard lock(mutex_);
        return phases_;
    }
    std::function<void()> onMark;

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::int64_t, bool> held_;
    std::vector<std::int64_t> phases_;
    std::vector<std::int64_t> marks_;
};

template<class Model, class Predicate>
void pumpUntil(Model& model, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        model.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("fixture owner pump deadline");
        std::this_thread::yield();
    }
}

template<class Model, class T>
void finish(Model& model, const Task<T>& task) {
    pumpUntil(model, [&] { return task.completed(); });
}

void returnsAndTransactions() {
    auto host = std::make_shared<Host>();
    host->hold(1);
    host->hold(2);
    App app(host);
    bool reentryRefused = false;
    host->onMark = [&] {
        try {
            (void)app.snapshot();
        } catch (const ExecutionFailure& error) {
            reentryRefused = std::string(error.what()).find("reentry") != std::string::npos;
        }
    };
    auto task = app.Run({}, false, false);
    pumpUntil(app, [&] { return host->entered(1); });
    require(!task.completed() && app.snapshot().returns == 0, "body did not really suspend");
    host->release(1);
    pumpUntil(app, [&] { return host->entered(2); });
    require(!task.completed() && app.pending() == 1, "return published before cleanup completed");
    require(app.snapshot().returns == 1 && app.snapshot().marker == 11,
        "return was not evaluated once before cleanup's publication boundary");
    require(reentryRefused, "cleanup synchronous host callback permitted model reentry");
    require(app.Edit(50).is_success(), "intervening owner edit failed");
    task.cancel();
    host->release(2);
    finish(app, task);
    require(task.result().is_success() && task.result().value() == 1,
        "late cancellation replaced an authoritative pending return");
    require(app.snapshot().returns == 1 && app.snapshot().marker == 150 && app.snapshot().cleaned == 1,
        "cleanup failed to refresh or published more than once");

    auto owned = app.OwnedReturn();
    finish(app, owned);
    require(owned.result().is_success() && owned.result().value() == "original",
        "return did not retain its owned value across cleanup mutation");
    require(app.snapshot().detail == "after suspension", "cleanup mutation was not published");
}

void failuresAndTransfers() {
    for (const bool primary : {false, true}) {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.Run({}, primary, true);
        finish(app, task);
        require(task.result().is_failure(), "cleanup failure was lost");
        require(task.result().error() == (primary ? "primary failure" : "cleanup failure"),
            "primary and secondary exception precedence changed");
        require(app.snapshot().marker == 11 && app.snapshot().cleaned == 0,
            "cleanup fault did not roll back only the uncommitted tail");
        require(host->phases() == std::vector<std::int64_t>({1, 2}), "fault skipped awaited cleanup");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.Transfers();
        finish(app, task);
        require(task.result().is_success() && task.result().value() == 1, "loop transfer target changed");
        require(host->phases() == std::vector<std::int64_t>({10, 11, 12, 20}),
            "break/continue did not unwind exactly once in lexical order");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto values = List<std::int64_t>::Empty();
        values.add(0);
        values.add(1);
        values.add(2);
        auto nested = app.NestedTransfers(values);
        finish(app, nested);
        require(nested.result().is_success() && nested.result().value() == 3,
            "foreach/while transfers or cleanup-local loops changed control flow");
        require(host->phases() == std::vector<std::int64_t>({50, 51, 52, 61, 62, 63, 70, 80, 71, 81}),
            "nested cleanup/fallthrough order changed");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.PropagatingLoop();
        finish(app, task);
        require(task.result().is_success() && task.result().value() == 2,
            "continue skipped the lowered for-loop iteration");
        require(host->phases() == std::vector<std::int64_t>({21, 22, 23}), "propagating loop cleanup order changed");
        auto propagated = app.PropagatedReturn();
        finish(app, propagated);
        require(propagated.result().is_failure() && propagated.result().error() == "propagated failure",
            "Result propagation did not route its pending return through cleanup");
        require(host->entered(24), "Result propagation skipped cleanup");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.Recover();
        finish(app, task);
        require(task.result().is_success() && task.result().value() == 8,
            "outer catch did not replace a return discarded by inner cleanup failure");
        require(app.snapshot().detail == "cleanup replaces return", "owned catch error did not survive suspension");
        require(host->phases() == std::vector<std::int64_t>({25, 26, 27}), "nested cleanup order changed");
    }
}

void cancellationAndShutdown() {
    {
        auto host = std::make_shared<Host>();
        host->hold(1);
        host->hold(40);
        App app(host);
        auto task = app.CleanupHelpers({});
        pumpUntil(app, [&] { return host->entered(1); });
        task.cancel();
        pumpUntil(app, [&] { return host->entered(40); });
        require(!task.completed(), "cancelled body skipped inherited source cleanup");
        task.cancel();
        host->release(40);
        finish(app, task);
        require(task.result().is_cancelled(), "pending cancellation value was replaced");
        require(host->phases() == std::vector<std::int64_t>({1, 40, 41, 42}),
            "interface/generic/virtual cleanup lost its inherited lease");
        require(app.snapshot().cleaned == 1, "source cleanup did not finish after cancellation");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        CancellationSource cleanup;
        cleanup.cancel();
        auto task = app.ExplicitCleanupToken(cleanup.token());
        finish(app, task);
        require(task.result().is_success() && app.snapshot().marker == 28,
            "explicit cleanup cancellation was ignored or implicitly propagated");
        require(!host->entered(28), "cancelled explicit cleanup token admitted host work");
    }
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.CleanupWait();
        task.cancel();
        app.poll();
        require(!task.completed(), "normal task cancellation cancelled a cleanup semaphore wait");
        require(app.ReleasePermit().is_success(), "cleanup semaphore release failed");
        finish(app, task);
        require(task.result().is_success() && app.snapshot().cleaned == 1, "cleanup semaphore did not resume");
    }
    {
        auto host = std::make_shared<Host>();
        host->hold(1);
        std::optional<IntTask> retained;
        {
            App app(host);
            retained = app.CleanupHelpers({});
            pumpUntil(app, [&] { return host->entered(1); });
        }
        require(retained->completed() && retained->result().is_cancelled(), "owner teardown did not settle admitted task");
        require(host->phases() == std::vector<std::int64_t>({1, 40, 41, 42}),
            "owner joined worker or destroyed services before asynchronous cleanup");
    }
    {
        auto host = std::make_shared<Host>();
        host->hold(2);
        std::optional<IntTask> retained;
        std::thread release;
        {
            App app(host);
            retained = app.Run({}, false, false);
            pumpUntil(app, [&] { return host->entered(2); });
            require(!retained->completed(), "cleanup did not suspend before owner destruction");
            release = std::thread([host] { host->release(2); });
        }
        release.join();
        require(retained->result().is_success(), "owner destruction abandoned a suspended cleanup return");
        require(host->phases() == std::vector<std::int64_t>({1, 2}), "owner destruction repeated suspended cleanup");
    }
    {
        ContextOwner context;
        auto host = std::make_shared<Host>();
        host->hold(1);
        InvocationContextScope invocation(context.capture().permit());
        App app(host);
        auto task = app.Run({}, false, false);
        pumpUntil(app, [&] { return host->entered(1); });
        context.invalidate();
        finish(app, task);
        require(task.result().is_cancelled(), "stale cleanup authority became a successful task");
        require(!host->entered(2), "cleanup shielding revived revoked host authority");
    }
}

void bounds() {
    {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.CleanupBudget();
        finish(app, task);
        require(task.result().is_failure() && task.result().error().find("cleanup evaluation budget") != std::string::npos,
            "real cleanup suspensions refilled the cleanup reserve");
        const auto calls = host->phases().size();
        require(calls > 1 && calls < 12, "cleanup exhaustion lacked repeated real suspension");
        auto exhausted = app.ExhaustedBody();
        finish(app, exhausted);
        require(exhausted.result().is_failure() && exhausted.result().error().find("source evaluation budget") != std::string::npos,
            "secondary cleanup fault replaced terminal normal exhaustion");
        require(host->entered(31) && app.snapshot().marker > 0,
            "exhausted body skipped cleanup or lost the documented publication boundary");
    }
    for (const auto& [depth, extra, success] : {std::tuple{30, false, true}, std::tuple{31, false, false}, std::tuple{30, true, false}}) {
        auto host = std::make_shared<Host>();
        App app(host);
        auto task = app.Depth(depth, extra);
        finish(app, task);
        require(task.result().is_success() == success, "cleanup source depth32/33 boundary changed");
        if (!success)
            require(task.result().error().find("source call depth") != std::string::npos, "depth failure lost its terminal identity");
        require(host->entered(32) == (depth == 30), "cleanup gained an extra source-call slot");
    }
}

}

int main() {
    try {
        returnsAndTransactions();
        failuresAndTransfers();
        cancellationAndShutdown();
        bounds();
#ifdef PIPELANG_ASYNC_FINALLY_PROJECT
        auto host = std::make_shared<Host>();
        FacetProbe facet(host);
        auto task = facet.Run({});
        finish(facet, task);
        require(task.result().is_success(), "project source-object facet lost async cleanup");
        require(host->phases() == std::vector<std::int64_t>({1, 40, 41, 42}), "source facet cleanup dispatch diverged");
#endif
        std::cout << "async finally: " << assertions << " source runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
