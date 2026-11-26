// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompositionGraph.hpp"

namespace pipelang::models {
struct Program;

// The caller must first link, normalize and check the complete ordinary program.
// No public manifest is used to reconstruct constructor/access/type information.
std::vector<CompositionGraph> planCompositions(const Program& program);
nlohmann::json compositionPlans(const Program& program);

}
