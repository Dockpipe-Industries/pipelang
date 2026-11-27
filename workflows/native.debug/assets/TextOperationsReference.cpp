// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace reference::source::NativeProof::TextOperations;

namespace {

pipelang::model::Result<std::int64_t> run(ReferenceApp& app, std::int64_t input)
{
    const auto task = app.Run(input);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference text application timed out");
        app.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    try {
        ReferenceApp app;
        if (run(app, 1).value() != 42)
            return 1;
        const auto failure = run(app, -1);
        if (!failure.is_failure() || failure.error() != "UTF-8 byte offset splits a scalar")
            return 1;
        std::cout << "The C++ reference agreed on UTF-8 text operations, scalar boundaries and byte limits.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
