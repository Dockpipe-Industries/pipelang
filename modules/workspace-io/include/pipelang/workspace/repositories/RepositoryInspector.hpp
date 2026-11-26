// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RepositoryData.hpp"
#include "../primitives/WorkspaceError.hpp"
#include <chrono>
#include <string>
#include <unordered_map>
namespace pipelang::workspace {
class RepositoryInspector final {
    std::unordered_map<std::string, std::string> admitted_;
    Cancellation cancelled_;
    std::string executable_;
    std::chrono::milliseconds budget_;
public:
    RepositoryInspector(std::unordered_map<std::string, std::string> admitted, Cancellation cancelled = {},
                        std::string executable = "/usr/bin/git", std::chrono::milliseconds budget = std::chrono::milliseconds(1500));
    RepositoryData inspect(const std::string& alias) const;
};
}
