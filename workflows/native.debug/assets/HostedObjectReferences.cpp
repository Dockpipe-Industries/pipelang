// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_OBJECT_CPP_REFERENCE
#include <Model.hpp>
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef PIPELANG_OBJECT_CPP_REFERENCE
using reference::source::NativeProof::HostedObjectReferences::Model;
#else
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_references;
#endif

void require(bool value, std::string_view reason)
{
    if (!value)
        throw std::runtime_error(std::string(reason));
}

#ifndef PIPELANG_OBJECT_CPP_REFERENCE
HostedValue checked(const HostedValueResult& result)
{
    require(result.is_success(), result.is_success() ? "" : result.error());
    return result.value();
}

class Model {
public:
    Model() : instance_(bind()) {}

    HostedValue Put(std::int64_t number)
    {
        const std::array<HostedArgument, 1> args{number};
        return checked(instance_.startValue(scope_, args).result());
    }

    HostedValue Temporary(std::int64_t number)
    {
        const std::array<HostedCallArgument, 1> args{number};
        return checked(instance_.startMethod(scope_, "Temporary", args).result());
    }

    HostedValueResult Inspect(const HostedValue& reference)
    {
        const std::array<HostedCallArgument, 1> args{reference};
        return instance_.startMethod(scope_, "Inspect", args).result();
    }

    HostedValue Current() { return checked(instance_.startMethod(scope_, "Current", {}).result()); }

    HostedValue Repeat(const HostedValue& item)
    {
        const std::array<HostedCallArgument, 1> args{item};
        return checked(instance_.startMethod(scope_, "Repeat", args).result());
    }

    void Drop() { (void)checked(instance_.startMethod(scope_, "Drop", {}).result()); }

    auto Wait(const HostedValue& item)
    {
        const std::array<HostedCallArgument, 1> args{item};
        return instance_.startMethod(scope_, "Wait", args);
    }

    auto Signal() { return instance_.startMethod(scope_, "Signal", {}).result(); }
    void poll() { scope_.poll(); }

private:
    static HostedInstance bind()
    {
        auto instance = HostedInstance::bind(proof_hosted_references, {});
        require(instance.is_success(), instance.is_success() ? "" : instance.error());
        return std::move(instance).value();
    }

    pipelang::model::TaskScope scope_;
    HostedInstance instance_;
};
#endif

template<class Task>
auto settle(Model& model, const Task& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        model.poll();
        require(std::chrono::steady_clock::now() < deadline, "native reference task did not complete");
        std::this_thread::yield();
    }
    return task.result();
}

int main()
{
    try {
        Model model;
        Model other;
#ifdef PIPELANG_OBJECT_CPP_REFERENCE
        const auto retained = model.Put(42).value();
        const auto current = model.Current();
        require(current.has_value() && current.value() == retained, "reference alias identity changed");
        const auto repeated = model.Repeat(retained).value();
        require(repeated.count() == 2 && repeated.at(0) == retained && repeated.at(1) == retained, "reference list lost aliases");
        require(model.Drop().is_success() && !model.Current().has_value(), "reference owner retained source field");
        const auto foreign = other.Inspect(retained);
        require(model.Inspect(retained).value() == 42 && foreign.is_failure() &&
                foreign.error() == "object belongs to a different owner domain", "reference owner validation changed");
        auto waiting = [&] {
            const auto temporary = model.Temporary(44);
            return model.Wait(temporary.value());
        }();
        require(!waiting.completed() && model.Signal().is_success(), "reference waiter did not suspend");
        require(settle(model, waiting).value() == 44, "reference task lost its object");
        auto offThread = model.Temporary(9).value();
        std::atomic<bool> releaseNow = false;
        std::jthread release([&releaseNow, released = std::move(offThread)] {
            while (!releaseNow.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)released;
        });
        releaseNow.store(true, std::memory_order_release);
        require(model.Temporary(10).is_success(), "reference allocation did not survive external release");
        release.join();
        for (int index = 0; index < 4200; ++index) {
            const auto temporary = model.Temporary(index);
            require(temporary.is_success() && model.Inspect(temporary.value()).value() == index,
                    "reference object graph did not reclaim released objects");
        }
        auto expired = [&] {
            Model binding;
            return binding.Temporary(17).value();
        }();
        bool expiredRejected = false;
        try {
            expired.require();
        } catch (const pipelang::model::ExecutionFailure& error) {
            expiredRejected = std::string_view(error.what()) == "object domain is closed";
        }
        require(expiredRejected, "reference survived its owner");
#else
        const auto retained = model.Put(42);
        require(model.Current().hasValue() && model.Current().value().sameObject(retained), "native alias identity changed");
        const auto repeated = model.Repeat(retained);
        require(repeated.size() == 2 && repeated.at(0).sameObject(retained) && repeated.at(1).sameObject(retained),
                "native reference list lost aliases");
        model.Drop();
        require(!model.Current().hasValue(), "native binding retained a cleared field");
        const auto foreign = other.Inspect(retained);
        require(checked(model.Inspect(retained)).integer() == 42 && foreign.is_failure() &&
                foreign.error() == "object belongs to a different owner domain", "native binding-owner validation changed");
        auto waiting = model.Wait(model.Temporary(44));
        require(!waiting.completed() && model.Signal().is_success(), "native reference waiter did not suspend");
        require(checked(settle(model, waiting)).integer() == 44, "native reference task lost its object");
        bool wrongThreadRejected = false;
        std::thread thread([&] {
            try {
                (void)retained.sameObject(retained);
            } catch (const pipelang::model::ExecutionFailure& error) {
                wrongThreadRejected = std::string_view(error.what()) == "object accessed from a different owner thread";
            }
        });
        thread.join();
        require(wrongThreadRejected, "native object reference escaped the owner thread");
        auto offThread = model.Temporary(9);
        std::atomic<bool> releaseNow = false;
        std::jthread release([&releaseNow, released = std::move(offThread)] {
            while (!releaseNow.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)released;
        });
        releaseNow.store(true, std::memory_order_release);
        require(checked(model.Inspect(model.Temporary(10))).integer() == 10, "native allocation did not survive external release");
        release.join();
        for (int index = 0; index < 4200; ++index) {
            const auto temporary = model.Temporary(index);
            require(checked(model.Inspect(temporary)).integer() == index, "native graph did not reclaim released objects");
        }
        auto expired = [&] {
            Model binding;
            return binding.Temporary(17);
        }();
        bool expiredRejected = false;
        try {
            (void)expired.sameObject(expired);
        } catch (const pipelang::model::ExecutionFailure& error) {
            expiredRejected = std::string_view(error.what()) == "object domain is closed";
        }
        const auto refused = model.Inspect(expired);
        require(expiredRejected && refused.is_failure() && refused.error() == "object domain is closed",
                "native object reference survived its owner");
#endif
        std::cout << "Hosted object references preserve aliases and reject foreign or expired bindings.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
