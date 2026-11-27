// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../support/support.hpp"

namespace pipelang::scalar {
inline constexpr char profile[] = "pipelang.cpp.float64-calls.v1";
struct Compilation {
    tools::Json request;
    tools::Json functions;
};
Compilation compile(const std::string& source, const std::string& package, const std::string& module);
}
