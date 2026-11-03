// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "SyncHost.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "PureHostValueChecks.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace pipelang::model;
using namespace pipelang_sync_host;
using IntResult = Result<std::int64_t>;

namespace {
class WorldAdapter final : public World {
public:
    IntResult Read(std::int64_t first, std::int64_t second) override {
        ownerCalls.push_back(std::this_thread::get_id());
        arguments.push_back(first);
        arguments.push_back(second);
        if (callback)
            callback();
        return IntResult::Success(first * 10 + second + externalValue);
    }

    IntResult Read(std::string key) override {
        ownerCalls.push_back(std::this_thread::get_id());
        if (callback)
            callback();
        if (key == "failure")
            return IntResult::Failure("host declined");
        if (key == "cancelled")
            return IntResult::Cancelled("host cancelled");
        if (key == "cancel-throw")
            throw OperationCancelled("cooperative cancellation");
        if (key == "throw")
            throw std::runtime_error("host exception");
        if (key == "unknown-throw")
            throw 7;
        if (key == "invalid-error")
            return IntResult::Failure(std::string("\xff", 1));
        return IntResult::Success(externalValue);
    }

    Result<HostValue> Exchange(HostValue input) override {
        ++exchanges;
        if (invalidPayload)
            input.text = std::string("\xff", 1);
        else
            input.text += " host-owned";
        return Result<HostValue>::Success(std::move(input));
    }

    Result<void> Touch(std::int64_t delta) override {
        ownerCalls.push_back(std::this_thread::get_id());
        externalValue += delta;
        return Result<void>::Success();
    }

    IntResult AsyncRead(std::int64_t input, CancellationToken cancellation) override {
        {
            std::lock_guard lock(workerMutex);
            worker = std::this_thread::get_id();
        }
        if (cancellation.is_cancelled())
            return IntResult::Cancelled("async cancelled");
        return IntResult::Success(input + 100);
    }

    std::int64_t externalValue = 0;
    bool invalidPayload = false;
    int exchanges = 0;
    std::function<void()> callback;
    std::vector<std::int64_t> arguments;
    std::vector<std::thread::id> ownerCalls;
    std::mutex workerMutex;
    std::thread::id worker;
};

template<class Callback>
void expectFailure(Callback callback, const char* expected) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure& error) {
        failed = true;
        assert(error.what() == std::string(expected));
    }
    assert(failed);
}

void directOrderingAndOutcomes() {
    auto host = std::make_shared<WorldAdapter>();
    SyncHostModel model(host);
    auto ordered = model.Ordered();
    assert(ordered.is_success() && ordered.value() == 12);
    assert((host->arguments == std::vector<std::int64_t>{1, 2}));
    assert(model.Inspect() == 2);
    assert(model.ReadAfterTouch().value() == 7 && host->externalValue == 7);
    assert(model.Forward("failure").is_failure());
    assert(model.Inspect() == 9); // Authored normal Failure still commits.
    assert(model.Ordered().is_success() && model.Inspect() == 2);
    assert(model.Forward("cancelled").is_cancelled());
    assert(model.Inspect() == 9); // Authored normal Cancelled still commits.
    for (const auto thread : host->ownerCalls)
        assert(thread == std::this_thread::get_id());
}

void validationOwnershipAndRollback() {
    auto host = std::make_shared<WorldAdapter>();
    SyncHostModel model(host);
    assert(model.Ordered().is_success());
    const auto before = model.snapshot();
    const auto failed = model.ThrowAfterTouch();
    assert(failed.is_failure() && failed.error() == "source fault after host effect");
    assert(model.snapshot() == before);
    assert(host->externalValue == 3); // External effects have no automatic rollback.

    HostValue input{4, "owned"};
    auto exchanged = model.Exchange(input);
    assert(exchanged.is_success() && exchanged.value().text == "owned host-owned");
    assert(input.text == "owned");
    const auto saved = model.snapshot();
    const auto exchanges = host->exchanges;
    const auto invalidArgument = model.Exchange(HostValue{5, std::string("\xff", 1)});
    assert(invalidArgument.is_failure() && invalidArgument.error() == "string contains invalid UTF-8");
    assert(host->exchanges == exchanges && model.snapshot() == saved);
    host->invalidPayload = true;
    const auto invalid = model.Exchange(input);
    assert(invalid.is_failure() && invalid.error() == "string contains invalid UTF-8");
    assert(model.snapshot() == saved);
    assert(model.Forward("invalid-error").error() == "string contains invalid UTF-8");
    assert(model.snapshot() == saved);
    const auto unknown = model.Forward("unknown-throw");
    assert(unknown.is_failure() && unknown.error() == "unexpected synchronous host service failure");
    assert(model.snapshot() == saved);
    const auto recovered = model.RecoverHostFault();
    assert(recovered.is_success() && recovered.value() == -1);
    assert(model.snapshot().notice == "host exception" && model.snapshot().cleanup == 1);
    const auto beforeCancellation = model.snapshot();
    const auto cancelled = model.CancelWithCleanup();
    assert(cancelled.is_cancelled() && cancelled.error() == "cooperative cancellation");
    assert(host->externalValue == 4); // Cleanup can call the host after guard unwind.
    assert(model.snapshot() == beforeCancellation);
    assert(model.Ordered().is_success());
    expectFailure([] { SyncHostModel refused(nullptr); }, "null host service");
}

void callbacksCannotReenterAndThreadsRemainConfined() {
    auto host = std::make_shared<WorldAdapter>();
    auto otherHost = std::make_shared<WorldAdapter>();
    SyncHostModel model(host);
    SyncHostModel other(otherHost);
    host->callback = [&] {
        constexpr auto refused = "model reentry during a synchronous host callback";
        expectFailure([&] { (void)model.Inspect(); }, refused);
        expectFailure([&] { (void)model.snapshot(); }, refused);
        expectFailure([&] { (void)model.Ordered(); }, refused);
        expectFailure([&] { (void)model.poll(); }, refused);
        assert(other.Ordered().is_success());
    };
    assert(model.Ordered().is_success());
    host->callback = {};
    std::thread outside([&] {
        expectFailure([&] { (void)model.Ordered(); }, "model accessed from a different owner thread");
        expectFailure([&] { (void)model.Inspect(); }, "model accessed from a different owner thread");
    });
    outside.join();
    assert(model.Ordered().is_success());
}

void injectedAdapterLifetimeIsOwned() {
    std::weak_ptr<WorldAdapter> weak;
    {
        auto host = std::make_shared<WorldAdapter>();
        weak = host;
        SyncHostModel model(host);
        host.reset();
        assert(!weak.expired());
        assert(model.ReadAfterTouch().value() == 7);
    }
    assert(weak.expired());
}

void mixedSyncAsyncKeepsDispatchContracts() {
    auto host = std::make_shared<WorldAdapter>();
    SyncHostModel model(host);
    auto task = model.Mixed(4, {});
    assert(!task.completed() && model.Inspect() == 3);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        assert(std::chrono::steady_clock::now() < deadline);
        model.poll();
        std::this_thread::yield();
    }
    assert(task.result().is_success() && task.result().value() == 142);
    assert(model.Inspect() == 142);
    for (const auto thread : host->ownerCalls)
        assert(thread == std::this_thread::get_id());
    std::lock_guard lock(host->workerMutex);
    assert(host->worker != std::this_thread::get_id() && host->worker != std::thread::id{});
}
}

int main() {
    pure_host_tests::run();
    directOrderingAndOutcomes();
    validationOwnershipAndRollback();
    callbacksCannotReenterAndThreadsRemainConfined();
    injectedAdapterLifetimeIsOwned();
    mixedSyncAsyncKeepsDispatchContracts();
}
