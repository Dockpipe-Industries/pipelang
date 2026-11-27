// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

template<class Start, class Poll>
int verifySelfEscape(const Start& start, const Poll& poll)
{
    for (const std::int64_t scenario : {0, 1, 2}) {
        const auto task = start(scenario);
        if (scenario == 2 && task.completed()) {
            const auto early = task.result();
            throw std::runtime_error(early.is_failure() ? early.error() : "receiver alias test did not suspend");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("receiver alias test did not complete");
            poll();
            if (!task.completed())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto result = task.result();
        if (!result.is_success())
            throw std::runtime_error(result.error());
        if (result.value() != 42)
            throw std::runtime_error("receiver alias test returned the wrong result");
        std::cout << result.value() << '\n';
    }
    return 0;
}
