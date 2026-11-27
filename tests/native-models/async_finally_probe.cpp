// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using Probe = async_finally_probe::source::AsyncCleanupProbe::Probe;
using PauseHost = async_finally_probe::source::AsyncCleanupProbe::PauseHost;
using namespace pipelang::model;

class Host final : public PauseHost {
public:
    Result<void> Pause(CancellationToken token) override {
        const int call = ++entered;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (released.load() < call && !token.is_cancelled()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<void>::Failure("probe gate deadline");
            std::this_thread::yield();
        }
        if (token.is_cancelled())
            return Result<void>::Cancelled("probe cancelled");
        return Result<void>::Success();
    }
    std::atomic<int> entered = 0;
    std::atomic<int> released = 0;
};

template<class Predicate>
void pump(Probe& probe, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        probe.poll();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("probe owner deadline");
        std::this_thread::yield();
    }
}

}

int main() {
    try {
        for (const bool cancel : {false, true}) {
            auto host = std::make_shared<Host>();
            Probe probe(host);
            auto task = probe.Run({});
            pump(probe, [&] { return host->entered.load() == 1; });
            if (task.completed())
                throw std::runtime_error("unchanged probe did not suspend its body");
            if (cancel)
                task.cancel();
            else
                host->released.store(1);
            pump(probe, [&] { return host->entered.load() == 2; });
            if (task.completed())
                throw std::runtime_error("unchanged probe returned before cleanup finished");
            task.cancel();
            host->released.store(2);
            pump(probe, [&] { return task.completed(); });
            // The unchanged probe deliberately maps any non-success body Result
            // to Failure. Its finally does not implicitly reinterpret Results.
            if (cancel ? !task.result().is_failure() :
                !(task.result().is_success() && task.result().value() == 7))
                throw std::runtime_error("unchanged probe result contract changed");
        }
        std::cout << "unchanged async cleanup probe: success and cancellation paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
