// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::ContextualRecursion::Session;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario, bool cancel = false)
{
    const auto task = session.Run(scenario);
    if (task.completed())
        throw std::runtime_error("reference recursion failed to suspend");
    if (cancel)
        task.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference recursion timed out");
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
    if (run(session, 4).value() != 14 || run(session, -3).value() != 17 ||
        run(session, 30).value() != 40 || run(session, 100).value() != 14)
        return 1;
    const auto limited = run(session, 31);
    if (!limited.is_failure() || limited.error() != "source call depth limit exceeded")
        return 1;
    if (!run(session, 3, true).is_cancelled() || run(session, 0).value() != 10)
        return 1;

    std::cout << "The C++ reference agreed on synchronous recursion, cleanup and the source depth limit.\n";
}
