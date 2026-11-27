// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/sdk-tools/bindings.hpp"

void nativeObservationContracts(const std::filesystem::path& prefix,
                                const std::filesystem::path& output);
std::string nativeObservationConsumer();
