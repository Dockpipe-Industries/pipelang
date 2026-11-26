// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <thread>

namespace {
int shared = 0;
}

int main()
{
    std::thread first([] { shared = 1; });
    std::thread second([] { shared = 2; });
    first.join();
    second.join();
    return shared == 0;
}
