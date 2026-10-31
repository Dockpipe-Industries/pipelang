// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Models.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace pipelang_models;
using pipelang::model::CancellationToken;
using pipelang::model::Result;
using PayloadResult = Result<Payload, std::string>;
using PayloadTask = pipelang::model::Task<PayloadResult>;

template<class T>
concept HasSecret = requires(T state) { state.secret; };
template<class T>
concept HasService = requires(T state) { state.Service; };
static_assert(!HasSecret<Model::State>);
static_assert(!HasService<Model::State>);

class ControlledProcessor final : public Processor {
public:
    PayloadResult Apply(Payload payload, CancellationToken cancellation) override {
        std::unique_lock lock(mutex);
        const auto sequence = received.size();
        received.push_back(payload);
        worker = std::this_thread::get_id();
        changed.notify_all();
        if (cooperateUntilCancelled) {
            lock.unlock();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!cancellation.is_cancelled()) {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            return PayloadResult::Cancelled("host cancelled");
        }
        const bool released = changed.wait_for(lock, std::chrono::seconds(3), [&] { return permits > sequence; });
        assert(released);
        if (cancellation.is_cancelled())
            return PayloadResult::Cancelled("host cancelled");
        if (invalidException)
            throw std::runtime_error(std::string("\xff", 1));
        if (invalidError)
            return PayloadResult::Failure(std::string("\xff", 1));
        if (invalidPayload)
            return PayloadResult::Success(Payload{1, std::string("\xff", 1)});
        if (throwFailure)
            throw std::runtime_error("host exception detail");
        if (fail)
            return PayloadResult::Failure("host failure detail");
        ++committed;
        changed.notify_all();
        return PayloadResult::Success(std::move(payload));
    }

    void waitFor(std::size_t count) {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return received.size() >= count; }));
    }
    void waitCommitted(std::size_t count) {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return committed >= count; }));
    }
    void release(std::size_t count) {
        std::lock_guard lock(mutex);
        permits = count;
        changed.notify_all();
    }
    std::vector<Payload> snapshots() {
        std::lock_guard lock(mutex);
        return received;
    }

    bool fail = false;
    bool throwFailure = false;
    bool invalidError = false;
    bool invalidPayload = false;
    bool invalidException = false;
    bool cooperateUntilCancelled = false;
    std::thread::id worker;
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t permits = 0;
    std::size_t committed = 0;
    std::vector<Payload> received;
};

void finish(Model& model, const PayloadTask& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void synchronousSemantics() {
    auto service = std::make_shared<ControlledProcessor>();
    Model model(service, Payload{});
    assert(model.Inspect().equal);
    assert(model.Defaults().value == 5 && model.Defaults().text == "initial");
    assert(model.Construct(9, "constructed") == (Payload{9, "constructed"}));
    assert(model.Minimum() == std::numeric_limits<std::int64_t>::min());
    assert(!model.ShortCircuit());
    assert(model.Advance().value() == 1);
    assert(model.ComposedEdit(11).is_success());
    assert(model.snapshot().count == 2);
    assert(model.snapshot().draft.value == 11);
    assert(model.snapshot().error == "edited");
    assert(model.Edit(std::numeric_limits<std::int64_t>::max(), "limit").is_success());
    auto before = model.snapshot();
    auto overflow = model.Overflow();
    assert(overflow.is_failure());
    assert(overflow.error() == "integer addition overflow");
    assert(model.snapshot() == before);
    assert(model.Secret() == 19);
    assert(model.Echo().value() == before.draft);
    assert(model.ExpectedFailure().is_failure());
    assert(model.snapshot().error == "expected detail");
    auto oversized = model.Edit(1, std::string(pipelang::model::maximumStringBytes + 1, 'a'));
    assert(oversized.is_failure());
    assert(model.snapshot().draft == before.draft);

    for (const auto& malformed : {std::string("\xff", 1), std::string("\xc0\xaf", 2),
                                  std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
                                  std::string("\xe2\x82", 2)}) {
        const auto saved = model.snapshot();
        auto result = model.Edit(1, malformed);
        assert(result.is_failure());
        assert(result.error() == "string contains invalid UTF-8");
        assert(model.snapshot() == saved);
    }
    assert(model.Edit(1, "Unicode: \xc3\xa9 \xf0\x9f\x8f\x8d").is_success());

    bool rejected = false;
    std::thread alien([&] {
        try {
            model.snapshot();
        } catch (const pipelang::model::ExecutionFailure&) {
            rejected = true;
        }
    });
    alien.join();
    assert(rejected);
}

void lazyTypedConditionalValues() {
    auto service = std::make_shared<ControlledProcessor>();
    Model model(service, Payload{});
    for (bool first : {false, true}) {
        const Payload left{4, "left"};
        const Payload right{9, "right"};
        assert(model.SelectInteger(first, -12, 41) == (first ? -12 : 41));
        assert(model.SelectBoolean(first, true, false) == first);
        assert(model.ValidationText(first) == (first ? "" : "Player name is required"));
        assert(model.SelectPayload(first, left, right) == (first ? left : right));
        const auto result = model.SelectResult(first, left);
        assert(result.is_success() == first);
        if (first)
            assert(result.value() == left);
        else
            assert(result.is_failure() && result.error() == "declined");
        for (bool second : {false, true}) {
            assert(model.ConditionalPrecedence(first, second) == ((first || second) ? 1 : 2));
            const auto expected = first ? 1 : (second ? 2 : 3);
            assert(model.ConditionalAssociation(first, second) == expected);
        }
    }
    assert(model.SafeConditional(true) == 17);
    assert(model.SafeAlternative(false) == 33);
    assert(model.ConditionalEffects(false).value() == 0);
    assert(model.snapshot().count == 1);
    assert(model.ConditionalEffects(true).value() == 3);
    assert(model.snapshot().count == 3);
    const auto before = model.snapshot();
    const auto failure = model.CheckedConditional(false);
    assert(failure.is_failure());
    assert(failure.error() == "integer addition overflow");
    assert(model.snapshot() == before);
    assert(model.CheckedConditional(true).value() == 5);
}

void ownedSnapshotsAndCapacity() {
    auto service = std::make_shared<ControlledProcessor>();
    Model model(service, Payload{1, "first"}, pipelang::model::TaskOptions{2});
    auto first = model.Process({});
    service->waitFor(1);
    assert(service->worker != std::this_thread::get_id());
    assert(model.Edit(2, "second").is_success());
    auto second = model.Process({});
    auto excess = model.Process({});
    assert(excess.completed() && excess.result().is_failure());
    assert(excess.result().error() == "task capacity exceeded");
    assert(model.pending() == 2);
    assert(model.Edit(3, "live").is_success());
    service->release(1);
    service->waitFor(2);
    finish(model, first);
    assert(first.result().value() == (Payload{1, "first"}));
    assert(model.snapshot().accepted == (Payload{1, "first"}));
    assert(model.snapshot().draft == (Payload{3, "live"}));
    assert(!second.completed());
    service->release(2);
    finish(model, second);
    assert(model.snapshot().accepted == (Payload{2, "second"}));
    assert(model.snapshot().completions == 2);
    assert(model.pending() == 0);
    assert(service->snapshots() == (std::vector<Payload>{{1, "first"}, {2, "second"}}));
}

void boundedQueueRefusalIsAnAwaitedFailure() {
    using VoidResult = Result<void, std::string>;
    pipelang::model::TaskScope scope(pipelang::model::TaskOptions{1});
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::atomic<int> secondCalls = 0;
    std::atomic<int> refusedCalls = 0;
    std::function<VoidResult()> blocking = [&] {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
        return VoidResult::Success();
    };
    auto first = scope.host<VoidResult>(std::move(blocking), {});
    assert(!first.await_ready());
    assert(first.await_suspend(std::noop_coroutine()));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
    }
    std::function<VoidResult()> queued = [&] {
        ++secondCalls;
        return VoidResult::Success();
    };
    auto second = scope.host<VoidResult>(std::move(queued), {});
    assert(second.await_suspend(std::noop_coroutine()));
    std::function<VoidResult()> refused = [&] {
        ++refusedCalls;
        return VoidResult::Success();
    };
    auto third = scope.host<VoidResult>(std::move(refused), {});
    assert(!third.await_suspend(std::noop_coroutine()));
    const auto failure = third.await_resume();
    assert(failure.is_failure());
    assert(failure.error() == "host queue unavailable");
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    std::size_t delivered = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (delivered < 2) {
        delivered += scope.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(delivered == 2);
    assert(first.await_resume().is_success());
    assert(second.await_resume().is_success());
    assert(secondCalls == 1);
    assert(refusedCalls == 0);
    assert(scope.poll() == 0);
}

void capturedHeapValuesSurviveWorkerAndFrameCleanup() {
    auto service = std::make_shared<ControlledProcessor>();
    service->release(16);
    Model model(service, Payload{});
    for (std::int64_t index = 0; index < 16; ++index) {
        const auto text = std::string(8192, static_cast<char>('a' + index));
        assert(model.Edit(index, text).is_success());
        pipelang::model::CancellationSource caller;
        auto task = model.Process(caller.token());
        finish(model, task);
        assert(task.result().is_success());
        assert(task.result().value() == (Payload{index, text}));
        assert(model.snapshot().accepted == (Payload{index, text}));
        assert(model.pending() == 0);
    }
    assert(service->snapshots().size() == 16);
}

void cancellationFailureAndSequentialAwait() {
    {
        auto service = std::make_shared<ControlledProcessor>();
        Model model(service, Payload{});
        auto task = model.Process({});
        service->waitFor(1);
        task.cancel();
        service->release(1);
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(model.snapshot().error == "host cancelled");
        assert(model.snapshot().completions == 1);
    }
    for (bool exception : {false, true}) {
        auto service = std::make_shared<ControlledProcessor>();
        service->fail = !exception;
        service->throwFailure = exception;
        Model model(service, Payload{});
        auto task = model.Process({});
        service->waitFor(1);
        service->release(1);
        finish(model, task);
        assert(task.result().is_failure());
        assert(model.snapshot().error == (exception ? "host exception detail" : "host failure detail"));
    }
    {
        auto service = std::make_shared<ControlledProcessor>();
        service->release(2);
        Model model(service, Payload{10, "sequence"});
        auto task = model.ProcessTwice({});
        finish(model, task);
        assert(task.result().is_success());
        assert(service->snapshots().size() == 2);
    }
    {
        auto service = std::make_shared<ControlledProcessor>();
        Model model(service, Payload{});
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        auto task = model.Process(cancellation.token());
        finish(model, task);
        assert(task.result().is_cancelled());
        assert(service->snapshots().empty());
    }
}

void malformedHostTextIsContained() {
    for (int mode = 0; mode < 3; ++mode) {
        auto service = std::make_shared<ControlledProcessor>();
        service->invalidError = mode == 0;
        service->invalidPayload = mode == 1;
        service->invalidException = mode == 2;
        Model model(service, Payload{});
        const auto accepted = model.snapshot().accepted;
        auto task = model.Process({});
        service->waitFor(1);
        service->release(1);
        finish(model, task);
        assert(task.result().is_failure());
        pipelang::model::validate(task.result().error());
        assert(model.snapshot().accepted == accepted);
        assert(model.snapshot().error == (mode == 2
            ? "host exception contained invalid error text" : "string contains invalid UTF-8"));
    }
}

void committedSuccessSurvivesTeardown() {
    auto service = std::make_shared<ControlledProcessor>();
    std::optional<PayloadTask> retained;
    {
        Model model(service, Payload{42, "committed"});
        retained = model.Process({});
        service->waitFor(1);
        service->release(1);
        service->waitCommitted(1);
        // Deliberately do not poll: successful external work is already done.
        assert(!retained->completed());
    }
    assert(retained->completed());
    assert(retained->result().is_success());
    assert(retained->result().value() == (Payload{42, "committed"}));

    auto nextService = std::make_shared<ControlledProcessor>();
    std::optional<PayloadTask> chained;
    {
        Model model(nextService, Payload{13, "first host committed"});
        chained = model.ProcessTwice({});
        nextService->waitFor(1);
        nextService->release(1);
        nextService->waitCommitted(1);
    }
    assert(chained->completed());
    assert(chained->result().is_cancelled());
    assert(nextService->snapshots().size() == 1);
}

void teardownAndStaleHandles() {
    auto service = std::make_shared<ControlledProcessor>();
    service->cooperateUntilCancelled = true;
    std::optional<PayloadTask> retained;
    {
        Model model(service, Payload{});
        retained = model.Process({});
        service->waitFor(1);
    }
    assert(retained->completed());
    assert(retained->result().is_cancelled());
    retained->cancel();
    auto replacementService = std::make_shared<ControlledProcessor>();
    Model replacement(replacementService, Payload{99, "new owner"});
    assert(replacement.poll() == 0);
    assert(replacement.snapshot().draft.value == 99);

    bool rejected = false;
    std::thread alien([&] {
        try {
            retained->completed();
        } catch (const pipelang::model::ExecutionFailure&) {
            rejected = true;
        }
    });
    alien.join();
    assert(rejected);
}

int main() {
    synchronousSemantics();
    lazyTypedConditionalValues();
    ownedSnapshotsAndCapacity();
    boundedQueueRefusalIsAnAwaitedFailure();
    capturedHeapValuesSurviveWorkerAndFrameCleanup();
    cancellationFailureAndSequentialAwait();
    malformedHostTextIsContained();
    committedSuccessSurvivesTeardown();
    teardownAndStaleHandles();
}
