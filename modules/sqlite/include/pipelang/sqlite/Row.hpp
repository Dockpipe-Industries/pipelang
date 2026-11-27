// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Value.hpp"

namespace pipelang::sqlite {
struct Row {
    std::vector<Value> values;
    bool operator==(const Row&) const = default;
};
}
