// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#ifndef PIPELANG_NUMERIC_FIXTURE
#define PIPELANG_NUMERIC_FIXTURE UInt32Values
#endif

int main(int argc, char** argv)
{
    if (argc != 2)
        return 64;
    const std::string_view input(argv[1]);
    std::int64_t scenario = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), scenario);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        return 64;

    try {
        reference::source::NativeProof::PIPELANG_NUMERIC_FIXTURE::ReferenceApp app;
        const auto task = app.Run(scenario);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return 70;
            app.poll();
            if (!task.completed())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto result = task.result();
        if (result.is_success()) {
            std::cout << result.value() << '\n';
            return 0;
        }
        std::cerr << result.error() << '\n';
        return result.is_cancelled() ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << pipelang::model::safeErrorMessage(error) << '\n';
        return 1;
    }
}
