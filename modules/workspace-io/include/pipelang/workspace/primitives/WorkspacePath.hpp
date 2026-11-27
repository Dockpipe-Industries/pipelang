// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "OwnedFd.hpp"
#include <sys/stat.h>
#include <string>
#include <vector>

namespace pipelang::workspace {
class WorkspacePath final {
public:
    static std::vector<std::string> components(const std::string& relative, bool allowRoot = false);
    static OwnedFd openAbsoluteDirectory(const std::string& absolute);
    static std::string identity(const struct stat& info);
    static bool sameFile(const struct stat& left, const struct stat& right);
    static bool unchanged(const struct stat& left, const struct stat& right);
    static void validateText(const std::string& text);
};
}
