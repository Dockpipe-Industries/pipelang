// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "syntax.hpp"
#include <functional>

namespace pipelang::models {

// Preserve the direct native-owner lowering while adding ordinary object
// semantics from the same normalized source declaration. Charge before copying.
void lowerProjectModelFacets(Program& program,
                            const std::function<void(const Declaration&)>& claimClone);

}
