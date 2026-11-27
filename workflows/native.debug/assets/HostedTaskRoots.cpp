// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using pipelang::model::CancellationToken;
#ifndef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_task_roots;
#endif

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class Observer final
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
    : public reference::source::NativeProof::HostedTaskRoots::Observer
#else
    : public HostedService
#endif
{
public:
    void hold()
    {
        std::lock_guard lock(mutex_);
        entered_.store(false, std::memory_order_release);
        held_ = true;
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        held_ = false;
        changed_.notify_all();
    }

    bool entered() const { return entered_.load(std::memory_order_acquire); }

#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
    pipelang::model::Result<std::int64_t> Pause(CancellationToken token) override
    {
        return wait(token);
    }
#else
    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.HostedTaskRoots.Observer";
    }

    pipelang::model::Result<HostedServiceValue> invoke(
        std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        require(signature == "Pause(CancellationToken)" && arguments.size() == 1, "unexpected observer method");
        using Result = pipelang::model::Result<HostedServiceValue>;
        const auto result = wait(std::get<CancellationToken>(arguments.front()));
        if (result.is_cancelled())
            return Result::Cancelled(result.error());
        if (result.is_failure())
            return Result::Failure(result.error());
        return Result::Success(result.value());
    }
#endif

private:
    pipelang::model::Result<std::int64_t> wait(const CancellationToken& token)
    {
        using Result = pipelang::model::Result<std::int64_t>;
        std::unique_lock lock(mutex_);
        entered_.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (held_ && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result::Failure("graph observer deadline");
            changed_.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (token.is_cancelled())
            return Result::Cancelled("graph observer cancelled");
        return Result::Success(0);
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> entered_{false};
    bool held_ = false;
};

class TestModel {
public:
    ~TestModel() { host->release(); }

    auto read(std::int64_t scenario)
    {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        return instance_.Run(scenario, {});
#else
        const std::array<HostedArgument, 2> arguments{scenario, CancellationToken{}};
        return instance_.start(scope_, arguments);
#endif
    }

    auto wait()
    {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        return instance_.Wait({});
#else
        const std::array<HostedCallArgument, 1> arguments{CancellationToken{}};
        return instance_.startMethod(scope_, "Wait", arguments);
#endif
    }

    void signal()
    {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        const auto result = instance_.Signal();
#else
        const auto result = instance_.startMethod(scope_, "Signal", {}).result();
#endif
        require(result.is_success(), result.is_success() ? "" : result.error());
    }

#ifndef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
    void requireOwningScope()
    {
        pipelang::model::TaskScope unrelated;
        const auto result = instance_.startMethod(unrelated, "Drop", {}).result();
        require(result.is_failure() && result.error() == "native hosted tasks must share an owning task scope",
                "concurrent root used an unrelated task scope");
    }
#endif

    void drop()
    {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        const auto result = instance_.Drop();
#else
        const auto result = instance_.startMethod(scope_, "Drop", {}).result();
#endif
        require(result.is_success(), result.is_success() ? "" : result.error());
    }

    void reset()
    {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        const auto result = instance_.Reset();
#else
        const auto result = instance_.startMethod(scope_, "Reset", {}).result();
#endif
        require(result.is_success(), result.is_success() ? "" : result.error());
    }

    template<class Predicate>
    void pump(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!predicate()) {
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
            instance_.poll();
#else
            scope_.poll();
#endif
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("hosted graph task did not settle");
            std::this_thread::yield();
        }
    }

    std::shared_ptr<Observer> host = std::make_shared<Observer>();

private:
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
    reference::source::NativeProof::HostedTaskRoots::Session instance_{host};
#else
    HostedInstance bind()
    {
        const std::array<HostedConstructorArgument, 1> args{std::shared_ptr<HostedService>(host)};
        auto model = HostedInstance::bind(proof_hosted_task_roots, args);
        require(model.is_success(), model.is_success() ? "" : model.error());
        return std::move(model).value();
    }
    pipelang::model::TaskScope scope_;
    HostedInstance instance_ = bind();
#endif
};

int main()
{
    try {
        TestModel model;
        const auto waiting = model.wait();
        require(!waiting.completed(), "semaphore task did not suspend");
        model.signal();
        model.pump([&] { return waiting.completed(); });
        const auto waitResult = waiting.result();
#ifdef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
        require(waitResult.is_success() && waitResult.value() == 44, "semaphore task could not resume");
#else
        require(waitResult.is_success() && waitResult.value().integer() == 44, "semaphore task could not resume");
#endif

        model.reset();
        for (std::int64_t scenario : {0, 1}) {
            model.host->hold();
            const auto reading = model.read(scenario);
            model.pump([&] { return model.host->entered(); });
            require(!reading.completed(), "graph read did not suspend");
#ifndef PIPELANG_MULTIPLE_TASKS_CPP_REFERENCE
            model.requireOwningScope();
#endif
            model.drop();
            model.host->release();
            model.pump([&] { return reading.completed(); });
            const auto result = reading.result();
            require(result.is_success() && result.value() == 42 + scenario,
                    result.is_success() ? "task lost its retained graph receiver" : result.error());
            model.reset();
        }

        model.host->hold();
        auto cancelled = model.read(1);
        auto surviving = model.read(1);
        model.pump([&] { return model.host->entered(); });
        model.drop();
        cancelled.cancel();
        model.pump([&] { return cancelled.completed(); });
        require(cancelled.result().is_cancelled(), "graph reader cancellation did not settle");
        model.host->release();
        model.pump([&] { return surviving.completed(); });
        require(surviving.result().is_success() && surviving.result().value() == 43,
                "cancellation released a receiver still held by another root");

        for (int index = 0; index < 4200; ++index) {
            model.reset();
            model.host->hold();
            const auto reading = model.read(index % 2);
            model.pump([&] { return model.host->entered(); });
            model.drop();
            model.host->release();
            model.pump([&] { return reading.completed(); });
            require(reading.result().is_success() && reading.result().value() == 42 + index % 2,
                    "completed graph roots exhausted or lost receiver storage");
        }
        std::cout << "Concurrent roots retain borrowed receivers and release them after completion or cancellation.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
