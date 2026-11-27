// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ChangeEvent.hpp"
#include <vector>
namespace pipelang::workspace {
struct ChangeBatch {
    std::vector<ChangeEvent> events;
    bool overflow = false;
};
}
