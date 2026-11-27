// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../support/support.hpp"
namespace pipelang::contained {
using namespace pipelang::tools;
inline constexpr std::uint64_t mebibyte = 1024 * 1024;
struct Request {
    bool job = false;
    fs::path output;
    fs::path estate;
    std::chrono::seconds timeout{900};
    std::uint64_t estateLimit = 8ULL * 1024 * mebibyte;
    std::vector<std::string> command;
};
fs::path currentGroup();
fs::path verifyJob();
void verifyInside(bool job);
int execute(const Request& request, const fs::path& executable);
int replace(const Request& request);
}
