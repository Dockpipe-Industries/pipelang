// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Type.hpp"
#include <cstddef>

namespace pipelang::models {

struct CompositionMethodSelector {
    Type owner;
    std::string method;
    std::size_t offset = 0;
};

}
