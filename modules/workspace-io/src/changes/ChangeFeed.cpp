// SPDX-License-Identifier: Apache-2.0
#include "ChangeFeed.hpp"
#include "../primitives/WorkspacePath.hpp"
#include <sys/inotify.h>
#include <poll.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace pipelang::workspace {
ChangeFeed::ChangeFeed(const FileIo& files) : files_(files), descriptor_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
    if (!descriptor_)
        throw WorkspaceError("watch_unavailable");
}
void ChangeFeed::watch(const std::string& directory) const {
    std::lock_guard guard(mutex_);
    auto pinned = files_.openDirectory(directory);
    for (const auto& [descriptor, path] : watches_) {
        (void)descriptor;
        if (path == directory)
            return;
    }
    if (watches_.size() >= 64)
        throw WorkspaceError("watch_limit");
    auto selected = "/proc/self/fd/" + std::to_string(pinned.get()) + "/.";
    constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                   IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_ONLYDIR;
    int descriptor = ::inotify_add_watch(descriptor_.get(), selected.c_str(), mask);
    if (descriptor < 0)
        throw WorkspaceError("watch_failed");
    watches_[descriptor] = directory;
}
void ChangeFeed::consume(const unsigned char* bytes, std::size_t length) const {
    std::size_t offset = 0;
    while (offset < length) {
        if (length - offset < sizeof(inotify_event))
            throw WorkspaceError("watch_invalid_event");
        inotify_event event{};
        std::memcpy(&event, bytes + offset, sizeof(event));
        if (event.len > length - offset - sizeof(inotify_event))
            throw WorkspaceError("watch_invalid_event");
        const char* nameBytes = reinterpret_cast<const char*>(bytes + offset + sizeof(inotify_event));
        offset += sizeof(inotify_event) + event.len;
        if (event.mask & IN_Q_OVERFLOW) {
            overflow_ = true;
            pending_.clear();
            continue;
        }
        auto found = watches_.find(event.wd);
        if (found == watches_.end())
            continue;
        if (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)) {
            overflow_ = true;
            if (!(event.mask & IN_IGNORED))
                ::inotify_rm_watch(descriptor_.get(), event.wd);
            watches_.erase(found);
            continue;
        }
        std::size_t nameLength = ::strnlen(nameBytes, event.len);
        if (nameLength == 0 || nameLength == event.len)
            continue;
        std::string name(nameBytes, nameLength);
        std::string path = found->second.empty() ? name : found->second + "/" + name;
        try {
            WorkspacePath::components(path);
        } catch (const WorkspaceError&) {
            overflow_ = true;
            continue;
        }
        std::string kind = "modified";
        if (event.mask & (IN_DELETE | IN_MOVED_FROM)) {
            kind = "deleted";
        } else if (event.mask & (IN_CREATE | IN_MOVED_TO)) {
            kind = "created";
        }
        if (pending_.size() >= 512) {
            overflow_ = true;
            pending_.clear();
        }
        pending_.push_back(ChangeEvent{std::move(path), std::move(kind)});
    }
}
ChangeBatch ChangeFeed::changes(std::int64_t maximumEvents, std::int64_t waitMilliseconds) const {
    if (maximumEvents < 1 || maximumEvents > 256 || waitMilliseconds < 0 || waitMilliseconds > 1000)
        throw WorkspaceError("limit");
    std::lock_guard guard(mutex_);
    (void)files_.openDirectory("");
    checkCancellation(files_.cancellation());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
    alignas(inotify_event) std::array<unsigned char, 65536> buffer{};
    std::size_t readBytes = 0;
    for (;;) {
        checkCancellation(files_.cancellation());
        auto count = ::read(descriptor_.get(), buffer.data(), buffer.size());
        if (count > 0) {
            consume(buffer.data(), static_cast<std::size_t>(count));
            readBytes += static_cast<std::size_t>(count);
            if (readBytes >= 262144) {
                overflow_ = true;
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno != EAGAIN)
            throw WorkspaceError("watch_failed");
        if (!pending_.empty() || overflow_ || std::chrono::steady_clock::now() >= deadline)
            break;
        struct pollfd ready{descriptor_.get(), POLLIN, 0};
        ::poll(&ready, 1, 10);
    }
    (void)files_.openDirectory("");
    ChangeBatch result;
    result.overflow = std::exchange(overflow_, false);
    std::size_t bytes = 0;
    while (!pending_.empty() && result.events.size() < static_cast<std::size_t>(maximumEvents)) {
        auto& event = pending_.front();
        if (bytes + event.path.size() + event.kind.size() > 65536) {
            result.overflow = true;
            pending_.clear();
            break;
        }
        bytes += event.path.size() + event.kind.size();
        result.events.push_back(std::move(event));
        pending_.pop_front();
    }
    return result;
}
}
