// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <iostream>
#include <stdexcept>

using reference::source::NativeProof::BlockingSemaphores::Session;

int main()
{
    try {
        Session session;
        for (const bool cancelled : {false, true, false}) {
            pipelang::model::CancellationSource cancellation;
            if (cancelled)
                cancellation.cancel();

            const auto result = session.Run(cancellation.token());
            if (cancelled) {
                if (!result.is_cancelled())
                    throw std::runtime_error("reference semaphore lost cancellation");
                std::cout << "cancelled\n";
            } else {
                if (!result.is_success())
                    throw std::runtime_error(result.error());
                if (result.value() != 42)
                    throw std::runtime_error("reference semaphore returned the wrong result");
                std::cout << result.value() << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
