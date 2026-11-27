// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace pipelang::workspace {
struct FileData {
    std::vector<std::uint8_t> bytes;
    std::string hash;
    std::string identity;
};
}
