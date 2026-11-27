// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pipelang::workspace {
class Hashing final {
public:
    static std::string bytes(const std::vector<std::uint8_t>& bytes);
    static std::string text(const std::string& text);
};
}
