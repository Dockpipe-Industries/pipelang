// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../support/support.hpp"
namespace pipelang::tools {
Json inventory(const fs::path& prefix, const Json& roots, const fs::path& excluded);
Json recordManifest(const fs::path& prefix, const fs::path& manifest, const Json& roots);
Json verifyManifest(const fs::path& prefix, const fs::path& manifest, const std::string& expected);
void exactHash(const std::string& value);
}
