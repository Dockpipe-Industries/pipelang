// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "manifest.hpp"

namespace pipelang::tools {
void packageSdk(const fs::path& prefix, const fs::path& manifest, const std::string& expected, const fs::path& output);
void extractSdk(const fs::path& manifest, const std::string& expected, const fs::path& output);
}
