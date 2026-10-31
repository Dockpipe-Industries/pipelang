// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "syntax.hpp"

namespace pipelang::models {

// Link a closed, caller-supplied graph without accessing files or the network.
Program linkModules(const std::vector<ModuleSource>& sources, const std::string& entry,
                    bool objectSyntax = false);

// Resolve explicit closed generic applications into ordinary owned-value ASTs.
// The ordinary checker remains responsible for all concrete expression semantics.
void normalizeComposition(Program& program);

}
