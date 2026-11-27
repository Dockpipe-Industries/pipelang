// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::PersistentReportValues::Session;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    const auto task = session.Run(scenario);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("persistent report reference timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    auto initial = pipelang::model::List<std::int64_t>::Empty();
    initial.add(42);
    const auto empty = pipelang::model::List<std::string>::Empty();
    Session first(initial, empty);
    Session second(initial, empty);
    if (run(first, 0).value() != 1 || run(first, 2).value() != 42)
        return 1;
    const auto failed = run(first, 1);
    if (!failed.is_failure() || failed.error() != "rollback persistent reports")
        return 1;
    if (run(first, 2).value() != 42 || run(second, 3).value() != 42)
        return 1;

    auto messages = pipelang::model::List<std::string>::Empty();
    for (std::size_t index = 0; index < 4000; ++index)
        messages.add(std::string(200, 'a'));
    Session bulk(initial, messages);
    if (run(bulk, 4).value() != 16000)
        return 1;
    std::cout << "The reference retained nested report values across invocations and rolled back a failed write.\n";
}
