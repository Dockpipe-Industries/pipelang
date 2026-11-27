// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompositionGraph.hpp"
#include <functional>

namespace pipelang::models {
struct Program;

struct CompositionFactoryBindings {
    std::string nameSpace;
    bool compiled = false;
    std::function<std::string(const Type&)> cpp;
    std::function<std::string(const std::string&)> callable;
    std::function<std::string(const Type&)> tag;
};

std::string emitCompositionFactory(const Program& program,
    const std::vector<CompositionGraph>& graphs, const CompositionFactoryBindings& bindings);

}
