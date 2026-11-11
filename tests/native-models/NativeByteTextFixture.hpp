// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeByteTextFixture(const std::filesystem::path& output);
void verifyNativeByteTextExports(const pipelang::tools::Json& manifest);
std::string nativeByteTextConsumer();
