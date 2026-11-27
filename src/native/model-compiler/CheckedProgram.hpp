// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "syntax.hpp"

namespace pipelang::models {

// Owns the program after linking, specialization, and semantic checking.
// Consumers take the checked definition without repeating frontend work.
class CheckedProgram final {
public:
    static CheckedProgram complete(Program program, bool enableObjects = false,
                                   std::vector<std::string> modelExports = {});

    static CheckedProgram project(const std::vector<ModuleSource>& modules,
                                  const std::string& entry,
                                  bool activeHostNumericEnvironment = false);

    const Program& definition() const
    {
        return program_;
    }

private:
    explicit CheckedProgram(Program program);

    Program program_;
};

}
