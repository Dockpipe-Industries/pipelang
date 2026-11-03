// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "manifest.hpp"

namespace pipelang::tools {

// Filesystem admission owns bytes and drift checks. Module/import syntax and
// source identities remain owned by the model compiler's parser.
class SourceProject {
public:
    static SourceProject load(const fs::path& projectRoot, const fs::path& sourceRoot,
                              const fs::path& entry);
    const std::vector<fs::path>& dependencies() const;
    Json request(const std::string& nameSpace) const;
    Json receipt() const;
    Json information() const;
    void verifyUnchanged() const;

private:
    fs::path projectRoot_;
    fs::path sourceRoot_;
    fs::path entryPath_;
    std::string entryModule_;
    Json modules_;
    Json index_;
    std::vector<fs::path> dependencies_;
};

Json projectInformation(const fs::path& prefix, const fs::path& manifest,
                        const std::string& manifestHash, const fs::path& projectRoot,
                        const fs::path& sourceRoot, const fs::path& entry);

}
