// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::Inheritance::Session;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario, bool cancel = false)
{
    const auto task = session.Run(scenario);
    if (task.completed())
        throw std::runtime_error("reference virtual call did not suspend");
    if (cancel)
        task.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference virtual call timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    Session session;
    constexpr std::array<std::int64_t, 5> expected{34, 89, 84, 37, 42};
    for (std::size_t scenario = 0; scenario < expected.size(); ++scenario) {
        if (run(session, static_cast<std::int64_t>(scenario)).value() != expected[scenario])
            return 1;
    }
    if (!run(session, 1, true).is_cancelled() || run(session, 1).value() != 89)
        return 1;

    std::cout << "The C++ reference agreed on inherited state, virtual calls, nullability and cancellation.\n";
}
