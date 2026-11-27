// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

void prepareLinkedRuntimeFixture(const std::filesystem::path& sdk,
                                 const std::filesystem::path& output,
                                 bool sanitizer);
