// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace reference::source::NativeProof::PersistentSets;
using namespace pipelang::model;

namespace {

Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    const auto task = session.Run(scenario);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference persistent set invocation timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void check(const Result<std::int64_t>& result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    if (result.value() != 42)
        throw std::runtime_error("unexpected reference persistent set result");
}

}

int main()
{
    try {
        auto initial = HashSet<std::string>::Empty();
        initial.add("seed");
        Session model(initial);
        check(run(model, 0));
        check(run(model, 1));
        const auto failed = run(model, 2);
        if (!failed.is_failure() || failed.error() != "rollback persistent sets")
            throw std::runtime_error("reference persistent set rollback lost the source fault");
        check(run(model, 1));
        check(run(model, 3));
        check(run(model, 4));
        Session other(initial);
        check(run(other, 5));
        for (std::size_t index = 0; index < 1100; ++index)
            initial.add("bulk-" + std::to_string(index));
        Session bulk(initial);
        check(run(bulk, 6));
        std::cout << "The C++ reference agreed on persistent HashSet state across calls.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
