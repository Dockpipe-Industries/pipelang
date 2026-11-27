// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace reference::source::NativeProof::HostedReceivers;

namespace {

pipelang::model::Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    const auto task = session.Run(scenario);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference nested receiver timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    try {
        const Paths paths{"src", "work", "out", "logs", "cache", "inputs", "state", "manifest.json"};
        const auto initial = pipelang::model::ByteBuffer::FromUtf8("initial");
        Session first(Inputs{40, Route{"étape", initial, true}, "", paths});
        Session second(Inputs{80, Route{"étape", initial, true}, "", paths});
        if (run(first, 0).value() != 42 || run(first, 2).value() != 44)
            return 1;
        const auto fault = run(first, 1);
        if (!fault.is_failure() || fault.error() != "nested receiver fault")
            return 1;
        if (run(first, 2).value() != 47 || run(second, 0).value() != 82)
            return 1;
        const auto synchronousFault = run(first, 3);
        if (!synchronousFault.is_failure() || synchronousFault.error() != "rollback synchronous writes")
            return 1;
        if (run(first, 5).value() != 0 || run(first, 4).value() != 52 || run(first, 6).value() != 53)
            return 1;
        if (run(first, 7).value() != 7 || run(first, 8).value() != 8)
            return 1;
        const auto textFault = run(first, 9);
        if (!textFault.is_failure() || textFault.error() != "rollback receiver text")
            return 1;
        if (run(first, 8).value() != 8 || run(first, 10).value() != 10 || run(first, 11).value() != 11 ||
            run(second, 12).value() != 12 || run(first, 13).value() != 32640)
            return 1;
        if (run(first, 14).value() != 14 || run(first, 15).value() != 15)
            return 1;
        const auto recordFault = run(first, 16);
        if (!recordFault.is_failure() || recordFault.error() != "rollback nested record fields" ||
            run(first, 15).value() != 15)
            return 1;
        if (run(first, 17).value() != 17 || run(first, 18).value() != 18)
            return 1;
        const auto replacementFault = run(first, 19);
        if (!replacementFault.is_failure() || replacementFault.error() != "rollback whole receiver records" ||
            run(first, 18).value() != 18)
            return 1;
        if (run(first, 20).value() != 42 || run(first, 21).value() != 42)
            return 1;
        const auto failed = run(first, 23);
        if (run(first, 22).value() != 42 || !failed.is_failure() || failed.error() != "negative score")
            return 1;
        std::cout << "The C++ reference agreed on synchronous effects, cleanup and rollback across receivers.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
