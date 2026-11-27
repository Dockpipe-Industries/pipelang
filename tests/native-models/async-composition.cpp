// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "AsyncComposition.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
using namespace pipelang_models;
using pipelang::model::CancellationToken;
using pipelang::model::Result;
using AsyncResult = Result<AsyncPayload, std::string>;
using AsyncTask = pipelang::model::Task<AsyncResult>;

class Controlled final : public AsyncHost {
public:
    AsyncResult Work(AsyncPayload value) override {
        std::unique_lock lock(mutex);
        received.push_back(value);
        changed.notify_all();
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
        committed = true;
        changed.notify_all();
        return AsyncResult::Success(value);
    }
    bool cooperate = false;
    AsyncResult WithToken(AsyncPayload value, CancellationToken token) override {
        if (cooperate) {
            {
                std::lock_guard lock(mutex);
                received.push_back(value);
                changed.notify_all();
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!token.is_cancelled()) {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            return AsyncResult::Cancelled("cooperative cancellation");
        }
        if (token.is_cancelled())
            return AsyncResult::Cancelled("explicit token cancelled");
        return Work(std::move(value));
    }
    void waitEntered() {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return !received.empty(); }));
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    void waitCommitted() {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return committed; }));
    }
    std::vector<AsyncPayload> captured() {
        std::lock_guard lock(mutex);
        return received;
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    bool committed = false;
    std::vector<AsyncPayload> received;
};

template<class Task>
void finish(AsyncComposition& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

int main() {
    {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{7, "captured"}, pipelang::model::TaskOptions{1});
        auto task = model.Root(0);
        host->waitEntered();
        assert(model.pending() == 1);
        assert(model.snapshot().stage == 11);
        assert(model.snapshot().handled == 0);
        assert(model.Edit(AsyncPayload{9, "live"}).is_success());
        host->release();
        finish(model, task);
        assert(task.result().is_success());
        assert(task.result().value() == (AsyncPayload{7, "captured handled"}));
        assert(host->captured() == (std::vector<AsyncPayload>{{7, "captured"}}));
        assert(model.snapshot().draft == (AsyncPayload{9, "live"}));
        assert(model.snapshot().accepted == (AsyncPayload{7, "captured"}));
        assert(model.snapshot().stage == 51);
        assert(model.snapshot().handled == 1);
        assert(model.pending() == 0);
    }
    {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{});
        pipelang::model::CancellationSource source;
        source.cancel();
        auto task = model.ChildCancellation(source.token());
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(host->captured().empty());
        assert(model.snapshot().stage == 77);
    }
    {
        auto host = std::make_shared<Controlled>();
        host->cooperate = true;
        std::optional<AsyncTask> retained;
        {
            AsyncComposition model(host, AsyncPayload{});
            retained = model.ClosingRoot(CancellationToken{});
            host->waitEntered();
        }
        // Closing host awaits complete immediately; they must consume the same
        // segment budget so an authored loop cannot prevent owner destruction.
        assert(retained->completed());
        assert(retained->result().is_failure());
        assert(retained->result().error().find("budget") != std::string::npos);
        assert(host->captured().size() == 1);
    }
    for (int mode : {1, 2, 3}) {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{1, "baseline"});
        auto task = model.Root(mode);
        if (mode == 3) {
            host->waitEntered();
            assert(model.Edit(AsyncPayload{2, "new live"}).is_success());
            host->release();
        }
        finish(model, task);
        assert(mode == 2 ? task.result().is_cancelled() : task.result().is_failure());
        assert(model.snapshot().stage == (mode == 3 ? 50 : 11));
        assert(model.snapshot().handled == 1);
        assert(!model.snapshot().detail.empty());
        assert(host->captured().size() == (mode == 3 ? 1 : 0));
    }
    {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{});
        pipelang::model::CancellationSource source;
        source.cancel();
        auto task = model.TokenRoot(source.token());
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(host->captured().empty());
        assert(model.snapshot().handled == 1);
    }
    {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{8, "authoritative"});
        auto task = model.Root(0);
        host->waitEntered();
        task.cancel();
        host->release();
        finish(model, task);
        assert(task.result().is_success());
        assert(model.snapshot().accepted.number == 8);
    }
    {
        auto host = std::make_shared<Controlled>();
        std::optional<AsyncTask> retained;
        {
            AsyncComposition model(host, AsyncPayload{4, "teardown"});
            retained = model.Root(0);
            host->waitEntered();
            host->release();
            host->waitCommitted();
            // No poll: destruction must drain the completion through both child
            // frames and the parent's authored success handling before cleanup.
        }
        assert(retained->completed());
        assert(retained->result().is_success());
        assert(retained->result().value().text == "teardown handled");
    }
    {
        auto host = std::make_shared<Controlled>();
        AsyncComposition model(host, AsyncPayload{});
        auto task = model.SharedBudget();
        finish(model, task);
        assert(task.result().is_failure());
        assert(task.result().error().find("budget") != std::string::npos ||
               task.result().error().find("limit") != std::string::npos);
        assert(host->captured().empty());
    }
}
