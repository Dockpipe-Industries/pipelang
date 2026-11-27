// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace reference::source::NativeProof::InterfaceDispatch;

namespace {

pipelang::model::Result<std::int64_t> run(ReferenceApp& app, auto entry, std::int64_t scenario)
{
    const auto task = (app.*entry)(scenario);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference interface application timed out");
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
        for (const auto entry : {&ReferenceApp::Run, &ReferenceApp::RunArray}) {
            if (run(app, entry, 1).value() != 119 || run(app, entry, 4).value() != 128)
                return 1;
            const auto failure = run(app, entry, -1);
            if (!failure.is_failure() || failure.error() != "interface request changed across suspension")
                return 1;
        }
        std::cout << "The C++ reference agreed on interface dispatch, object identity and returned failure.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
