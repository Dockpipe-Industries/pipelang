// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "manifest.hpp"
#include "../model-compiler/syntax.hpp"

namespace pipelang::tools {
// Private indexed-file value. Files retain their own bytes and parser headers;
// grouping never rewrites source or turns fragment offsets into virtual offsets.
struct SourceProjectFragment {
    fs::path path;
    std::string source;
    models::Program header;
    bool explicitModule;
};
}
