// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "manifest.hpp"

namespace pipelang::tools {
// One closed filesystem input inventory. Language imports and exported model
// identities are admitted by the compiler, never resolved by the SDK loader.
class SourceSet {
public:
    static SourceSet load(const fs::path& descriptor);
    const std::vector<fs::path>& dependencies() const;
    const std::string& compilerProtocol() const;
    Json request(const std::string& nameSpace) const;
    Json receipt() const;
    Json information() const;
    void verifyUnchanged() const;

private:
    fs::path descriptor_;
    std::string descriptorBytes_;
    std::string compilerProtocol_;
    std::string entry_;
    Json models_;
    Json modules_;
    Json records_;
    std::vector<fs::path> dependencies_;
    std::vector<std::string> contents_;
};

Json sourceSetInformation(const fs::path& prefix, const fs::path& manifest,
                          const std::string& manifestHash, const fs::path& descriptor);
}
