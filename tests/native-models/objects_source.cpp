// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Objects.hpp"
#include "ObjectModules.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
using object_models::App;
using IntResult = pipelang::model::Result<std::int64_t, std::string>;
using IntTask = pipelang::model::Task<IntResult>;

class ImmediateHost final : public object_models::ProgressHost {
public:
    IntResult Step() override {
        calls.fetch_add(1);
        return IntResult::Success(19);
    }
    std::atomic<int> calls{0};
};

template<class Task>
void finish(App& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void aliasesAndTransactions() {
    auto host = std::make_shared<ImmediateHost>();
    App model(host);
    assert(model.Identities().value());
    assert(model.Overloads().value() == 1327);
    assert(model.InterfaceMutation().value() == 69);
    assert(model.Cycles().value());
    const auto readout = model.OwnedReadout().value();
    assert(readout.count() == 2 && readout.at(0) == 23 && readout.at(1) == 23);
    assert(model.CaughtPartial().value() == 13);
    assert(model.FirstValue().value() == 13 && model.AliasValue().value() == 13);
    const auto before = model.snapshot();
    auto failure = model.UncaughtGraph();
    assert(failure.is_failure() && failure.error() == "uncaught object fault");
    assert(model.snapshot() == before);
    assert(model.FirstValue().value() == 13 && model.AliasValue().value() == 13);
    assert(model.ConstructorSavepoint().value() == 5);
    assert(model.FirstValue().value() == 5 && model.AliasValue().value() == 5);
    assert(model.SharedConstructorEffects().value() == 1);
    assert(model.UncaughtConstructor().is_failure());
    assert(model.FirstValue().value() == 5 && model.AliasValue().value() == 5);
    assert(model.SharedConstructorEffects().value() == 2);
    const auto limited = model.DispatchBudget();
    assert(limited.is_failure());
    assert(limited.error().find("budget") != std::string::npos);
    assert(model.snapshot() == before && model.FirstValue().value() == 5);
    auto exported = model.Export();
    assert(exported.is_success());
    App other(host);
    assert(other.ReadExternal(exported.value()).is_failure());
}

void capturedReceiverAndProgress() {
    auto host = std::make_shared<ImmediateHost>();
    App model(host);
    auto waiting = model.CapturedReceiver();
    assert(!waiting.completed() && model.pending() == 1);
    assert(model.ChangeWhileWaiting().is_success());
    assert(model.FirstValue().value() == 50 && model.AliasValue().value() == 7);
    auto probe = model.Probe();
    finish(model, probe);
    assert(probe.result().is_success() && probe.result().value() == 19);
    assert(host->calls.load() == 1 && !waiting.completed());
    assert(model.ReleaseSignal().is_success());
    finish(model, waiting);
    assert(waiting.result().is_success() && waiting.result().value() == 7);
    assert(model.FirstValue().value() == 50 && model.AliasValue().value() == 7);
    assert(model.pending() == 0);
}

void closingFrames() {
    auto host = std::make_shared<ImmediateHost>();
    std::optional<IntTask> retained;
    {
        App model(host);
        retained = model.CapturedReceiver();
        assert(!retained->completed());
    }
    assert(retained->completed() && retained->result().is_cancelled());

    using ExportResult = decltype(std::declval<App&>().Export());
    std::optional<ExportResult> closed;
    {
        App origin(host);
        closed = origin.Export();
        assert(closed->is_success());
    }
    App receiver(host);
    assert(receiver.ReadExternal(closed->value()).is_failure());
}

int main() {
    object_modules::App modular;
    assert(modular.Read().value() == 31);
    {
        App model(std::make_shared<ImmediateHost>());
        auto task = model.CommittedBeforeChild();
        finish(model, task);
        assert(task.result().is_success() && task.result().value() == 17);
        assert(model.snapshot().marker == 17);
    }
    {
        App model(std::make_shared<ImmediateHost>());
        auto task = model.CommittedBeforeLocalChild();
        finish(model, task);
        assert(task.result().is_success() && task.result().value() == 46);
        assert(model.snapshot().marker == 23 && model.FirstValue().value() == 42);
    }
    aliasesAndTransactions();
    capturedReceiverAndProgress();
    closingFrames();
}
