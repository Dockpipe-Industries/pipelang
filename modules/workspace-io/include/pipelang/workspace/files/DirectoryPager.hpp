// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DirectoryPage.hpp"
#include "FileIo.hpp"
namespace pipelang::workspace {
class DirectoryPager final {
    const FileIo& files_;
public:
    explicit DirectoryPager(const FileIo& files) : files_(files) {}
    DirectoryPage list(const std::string& directory, const std::string& after, std::int64_t limit) const;
};
}
