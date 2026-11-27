// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include "SelfEscapeCheck.hpp"

int main()
{
    try {
        reference::source::NativeProof::SelfEscape::Session instance;
        return verifySelfEscape([&instance](std::int64_t scenario) { return instance.Run(scenario); },
                                [&instance] { instance.poll(); });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
