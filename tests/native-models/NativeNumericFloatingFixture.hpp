// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeNumericFloatingFixture(const std::filesystem::path& output);
void verifyNativeNumericFloatingExports(const pipelang::tools::Json& manifest);
std::string nativeNumericFloatingConsumer();
