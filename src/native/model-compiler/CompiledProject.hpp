// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

namespace pipelang::models {

struct CompiledProject {
    std::string header;
    std::map<std::string, std::string> files;
};

}
