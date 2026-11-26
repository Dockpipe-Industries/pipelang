// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "FloatingObservationCheck.hpp"
#include "Model.hpp"

#include <iostream>

int main()
{
    try {
        reference::source::NativeProof::FloatingObservation::Session session;
        checkFloatingObservation([&] { return session.Run(); });
        std::cout << "C++ floating observation preserved rounding modes and exception flags.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
