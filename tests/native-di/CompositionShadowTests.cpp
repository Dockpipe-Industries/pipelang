// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include DI_SHADOW_FACTORY_HEADER
#include <iostream>

int main() {
    auto root = di_shadow::composition::construction::construction::Create();
    if (!root.is_success())
        return 1;
    auto service = root.value()->ResolveService();
    if (!service.is_success())
        return 1;
    auto observed = service.value().Read();
    if (!observed.is_success() || observed.value() != 7 || !root.value()->Close().is_success())
        return 1;
    std::cout << "actual construction namespace shadow passed\n";
    return 0;
}
