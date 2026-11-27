// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeNumericEnvironmentFixture(const std::filesystem::path& output);
void verifyNativeNumericEnvironmentExports(const pipelang::tools::Json& manifest);
std::string nativeNumericEnvironmentConsumer();
