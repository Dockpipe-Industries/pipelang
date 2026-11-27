// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DirectoryEntry.hpp"
#include <vector>
namespace pipelang::workspace {
struct DirectoryPage {
    std::vector<DirectoryEntry> entries;
    std::string nextAfter;
    bool complete = false;
};
}
