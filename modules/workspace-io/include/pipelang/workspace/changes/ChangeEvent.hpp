// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
namespace pipelang::workspace {
struct ChangeEvent {
    std::string path;
    std::string kind;
};
}
