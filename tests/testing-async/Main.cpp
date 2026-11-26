// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "FixturePorts.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

int main() {
    try {
        using Model = pipelang_testing_async::source::PipeLang::Testing::AsyncSelfTests::AsyncSuite;
        using Outcome = pipelang_testing_async::source::PipeLang::Testing::TestOutcome;
        auto fixture = std::make_shared<FixturePorts>();
        pipelang::model::CancellationSource preCancelled;
        preCancelled.cancel();
        Model model(fixture, fixture);
        auto task = model.Run(fixture->bodyCancellation.token(), preCancelled.token());
        fixture->rootReleased = true;
        const auto watchdog = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!task.completed()) {
            model.poll();
            if (std::chrono::steady_clock::now() >= watchdog)
                throw std::runtime_error("async source suite exceeded the native fixture watchdog");
            std::this_thread::yield();
        }
        const auto result = task.result();
        if (!result.is_success())
            throw std::runtime_error(result.error());
        bool failed = false;
        for (const auto& item : result.value().values()) {
            if (item.test.outcome == Outcome::Passed)
                continue;
            failed = true;
            std::cerr << item.test.id << ": ";
            if (item.test.primaryFailure.has_value())
                std::cerr << item.test.primaryFailure.value().code << " " << item.test.primaryFailure.value().message;
            if (item.test.cleanupFailure.has_value())
                std::cerr << " cleanup: " << item.test.cleanupFailure.value().code << " "
                          << item.test.cleanupFailure.value().message;
            if (item.cleanupClockFailure.has_value())
                std::cerr << " clock: " << item.cleanupClockFailure.value().code << " "
                          << item.cleanupClockFailure.value().message;
            std::cerr << '\n';
        }
        if (failed)
            return 1;
        std::cout << "PipeLang source async test suite: " << result.value().count() << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
