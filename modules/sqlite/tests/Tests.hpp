// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "TestContext.hpp"

void Functional(TestContext& context);
void Adversarial(TestContext& context);
void Backup(TestContext& context);

inline void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
