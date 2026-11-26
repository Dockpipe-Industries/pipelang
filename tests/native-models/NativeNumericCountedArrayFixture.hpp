// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeNumericCountedArrayFixture(const std::filesystem::path& output);
void verifyNativeNumericCountedArrayExports(const pipelang::tools::Json& manifest);
void verifyNativeNumericCountedArrayRejections(const std::filesystem::path& prefix,
                                              const pipelang::tools::Json& request);
std::string nativeNumericCountedArrayConsumer();
