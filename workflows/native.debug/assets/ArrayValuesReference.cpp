// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::ArrayValues::Session;

namespace {

pipelang::model::Result<pipelang::model::Array<std::string>> run(Session& model, std::int64_t size)
{
    const auto task = model.Run(size);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("array reference timed out");
        model.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    Session session;
    for (const auto input : {0, 4, 11, 12, 17, 4096}) {
        const auto result = run(session, input);
        if (!result.is_success() || result.value().count() != 2 || result.value().at(0) != "new" ||
            result.value().at(1) != "old")
            return 1;
    }
    for (const auto size : {-1, 18, 4097}) {
        const auto result = run(session, size);
        if (!result.is_failure() || result.error() != "array size outside admitted range")
            return 1;
    }
    for (const auto input : {14, 15, 16}) {
        const auto result = run(session, input);
        if (!result.is_failure() || result.error() != "collection index outside admitted range")
            return 1;
    }
    const auto oversized = run(session, 13);
    if (!oversized.is_failure() || oversized.error() != "collection owned byte limit exceeded")
        return 1;
    std::cout << "The C++ reference agreed on array snapshots, bounds, and owned byte limits.\n";
}
