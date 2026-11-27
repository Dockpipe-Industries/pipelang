// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ChangeBatch.hpp"
#include "../files/FileIo.hpp"
#include <deque>
#include <mutex>
#include <unordered_map>
namespace pipelang::workspace {
class ChangeFeed final {
    const FileIo& files_;
    OwnedFd descriptor_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<int, std::string> watches_;
    mutable std::deque<ChangeEvent> pending_;
    mutable bool overflow_ = false;
    void consume(const unsigned char* bytes, std::size_t length) const;
public:
    explicit ChangeFeed(const FileIo& files);
    void watch(const std::string& directory) const;
    ChangeBatch changes(std::int64_t maximumEvents, std::int64_t waitMilliseconds) const;
};
}
