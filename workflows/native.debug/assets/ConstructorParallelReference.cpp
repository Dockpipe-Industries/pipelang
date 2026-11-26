// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace model = pipelang::model;
using Session = reference::source::NativeProof::ConstructorParallel::Session;

bool rejected(std::int64_t mode, std::size_t degree, std::string_view expected)
{
    try {
        Session session(mode, {}, {degree});
    } catch (const model::ExecutionFailure& failure) {
        return std::string_view(failure.what()) == expected;
    }
    return false;
}

int main()
{
    try {
        if (!rejected(1, 2, "constructor worker failed") ||
            !rejected(2, 1, "constructor worker handoff timed out"))
            throw std::runtime_error("reference constructor lost worker failure or degree");
        Session retried(0, {}, {1});
        Session parallel(2, {}, {2});
        if (retried.Run() != 42 || parallel.Run() != 42)
            throw std::runtime_error("reference constructor did not settle its shared cells");
        std::cout << "Reference constructor workers honored shared cells, failure, and configured degree.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
