// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include "HostedInheritanceCheck.hpp"

int main()
{
    try {
        reference::source::NativeProof::HostedInheritance::ReferenceHarness instance(9, 12);
        return verifyHostedInheritance([&instance](std::int64_t scenario) { return instance.Run(scenario); },
                                       [&instance] { instance.poll(); });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
