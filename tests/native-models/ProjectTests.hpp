// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

void generateProjectFixture(const std::filesystem::path& prefix, const std::filesystem::path& output);
void projectContracts(const std::filesystem::path& prefix, const std::filesystem::path& output);

void generateCompiledProjectFixture(const std::filesystem::path& prefix, const std::filesystem::path& output);

void sourceLibraryContracts(const std::filesystem::path& prefix, const std::filesystem::path& output);
