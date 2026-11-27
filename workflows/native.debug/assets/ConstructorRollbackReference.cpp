// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <array>
#include <iostream>

using reference::source::NativeProof::ConstructorRollback::Session;

int main()
{
    pipelang::model::TaskOptions options;
    options.evaluationStepsPerSegment = 33554432;
    Session session(options);
    constexpr std::array<std::int64_t, 5> expected{42, 77, 77, -1, 77};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto result = session.Run(static_cast<std::int64_t>(index));
        if (index == 3) {
            if (!result.is_failure() || result.error() != "outer invocation failed")
                return 1;
            continue;
        }
        if (!result.is_success() || result.value() != expected[index])
            return 1;
    }
    std::cout << "The C++ reference agreed on construction rollback, retries and later segment failure.\n";
}
