// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "manifest.hpp"

namespace pipelang::tools {

Json compiledProjectPlan(const Json& implementation, bool withContents);
std::set<std::string> compiledProjectOwnedNames(const fs::path& output);
void publishCompiledProject(const fs::path& output, const std::map<std::string, std::string>& files);

}
