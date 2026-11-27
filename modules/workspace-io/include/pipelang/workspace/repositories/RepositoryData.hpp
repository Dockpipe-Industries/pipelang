// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
namespace pipelang::workspace {
struct RepositoryData {
    std::string alias;
    std::string checkoutIdentity;
    std::string commonIdentity;
    std::string origin;
};
}
