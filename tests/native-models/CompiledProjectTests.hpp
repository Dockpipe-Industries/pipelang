// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

void compiledProjectContracts(const std::filesystem::path& prefix, const std::filesystem::path& output);

void compareCompiledProjectBaseline(const std::filesystem::path& baselineCompiler,
                                    const std::filesystem::path& candidateSdk,
                                    const std::filesystem::path& output);

void verifyCompiledProjectPolicy(const std::filesystem::path& build, bool sanitizer);
