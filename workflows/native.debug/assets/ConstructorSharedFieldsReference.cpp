// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <iostream>

int main()
{
    reference::source::NativeProof::ConstructorSharedFields::Session session;
    const auto result = session.Run();
    if (!result.is_success()) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    std::cout << result.value() << '\n';
    return result.value() == 111 ? 0 : 1;
}
