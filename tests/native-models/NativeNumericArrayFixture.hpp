// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeNumericArrayFixture(const std::filesystem::path& output);
void verifyNativeNumericArrayExports(const pipelang::tools::Json& manifest);
std::string nativeNumericArrayConsumer();
void verifyNativeNumericArrayRejections(const std::filesystem::path& prefix,
                                       const pipelang::tools::Json& request);
std::string nativeNumericArrayOrdinaryConsumer();
void verifyNativeNumericArrayOrdinaryFixture(const std::filesystem::path& prefix,
                                            const std::filesystem::path& output);
