// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pipelang::models {

struct Type {
    std::string name;
    std::vector<Type> arguments;
    std::string sourceName = {};
    std::optional<std::size_t> fixedExtent = {};
    std::string text() const;
    std::string displayText() const;
    bool operator==(const Type& other) const {
        return name == other.name && arguments == other.arguments && fixedExtent == other.fixedExtent;
    }
};

}
