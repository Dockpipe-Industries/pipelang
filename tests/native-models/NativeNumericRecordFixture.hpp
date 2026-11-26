// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../src/native/sdk-tools/bindings.hpp"

void writeNativeNumericRecordFixture(const std::filesystem::path& output);
void verifyNativeNumericRecordExports(const pipelang::tools::Json& manifest);
std::string nativeNumericRecordConsumer();
