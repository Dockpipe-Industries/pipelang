// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>

extern "C" std::int64_t nativeSelect(std::int64_t input) asm("Select.Selection.Select");

namespace {

bool check(std::int64_t input)
{
    using namespace pipelang::model;

    TaskScope scope;
    EvaluationBudget budget;
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    ObjectExecution execution(scope, budget, transaction, {});

    const std::int64_t expected = input < 39 ? 7 : 42;
    const auto native = nativeSelect(input);
    const auto reference = reference::pl_object_function_0(execution, input);
    if (native == expected && reference == expected)
        return true;

    std::cerr << "Select(" << input << ") expected " << expected
              << ", machine code returned " << native << ", C++ returned " << reference << '\n';
    return false;
}

}

int main()
{
    for (const auto input : {std::numeric_limits<std::int64_t>::min(),
                            std::numeric_limits<std::int64_t>::max()}) {
        if (!check(input))
            return 1;
    }

    for (std::int64_t input = 30; input <= 48; ++input) {
        if (!check(input))
            return 1;
    }

    std::mt19937_64 random(0x50495045);
    for (int sample = 0; sample < 1024; ++sample) {
        if (!check(std::bit_cast<std::int64_t>(random())))
            return 1;
    }

    std::cout << "Select agreed with independent expectations and the C++ backend for 1045 inputs\n";
}
