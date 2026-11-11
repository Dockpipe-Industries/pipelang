// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
namespace pipelang::workspace {
class Uuid final {
public:
    static std::string create();
};
}
