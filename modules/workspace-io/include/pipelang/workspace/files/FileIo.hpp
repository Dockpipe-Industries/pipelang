// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FileData.hpp"
#include "../primitives/OwnedFd.hpp"
#include "../primitives/WorkspaceError.hpp"
#include <sys/stat.h>
#include <mutex>
#include <string>

namespace pipelang::workspace {
class FileIo final {
    std::string home_;
    OwnedFd root_;
    struct stat rootIdentity_{};
    Cancellation cancelled_;
    mutable std::timed_mutex mutation_;
    void verifyRoot() const;
    FileData readAt(int directory, const std::string& name, struct stat* identity = nullptr) const;
    void publish(const std::string& path, const std::vector<std::uint8_t>& bytes,
                 const std::string* expectedHash) const;
public:
    explicit FileIo(std::string home, Cancellation cancelled = {});
    const Cancellation& cancellation() const { return cancelled_; }
    int rootDescriptor() const { return root_.get(); }
    OwnedFd openDirectory(const std::string& relative) const;
    FileData read(const std::string& relative) const;
    void writeNew(const std::string& relative, const std::vector<std::uint8_t>& bytes) const;
    void replace(const std::string& relative, const std::string& expectedHash,
                 const std::vector<std::uint8_t>& bytes) const;
    void makeDirectory(const std::string& relative) const;
    std::string resolve(const std::string& relative) const;
};
}
