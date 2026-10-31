// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "manifest.hpp"
namespace pipelang::tools {
void admitModel(const Json& manifest, const std::string& nameSpace);
void requireViewCompatible(const Json& manifest);
Json generateModelBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                           const fs::path& modelTool, const fs::path& model,
                           const std::string& nameSpace, const fs::path& output);
Json generateBindings(const fs::path& prefix, const fs::path& manifest, const std::string& manifestHash,
                      const fs::path& modelTool, const fs::path& viewTool,
                      const fs::path& model, const fs::path& view, const std::string& nameSpace,
                      const fs::path& output, bool styleInspection);
}
