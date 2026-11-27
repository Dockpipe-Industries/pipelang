// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
namespace pipelang::workspace {
struct DirectoryEntry {
    std::string path;
    std::string name;
    bool directory = false;
    std::string identity;
};
}
