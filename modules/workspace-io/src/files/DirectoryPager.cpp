// SPDX-License-Identifier: Apache-2.0
#include "DirectoryPager.hpp"
#include "../primitives/Hashing.hpp"
#include "../primitives/WorkspacePath.hpp"
#include <dirent.h>
#include <fcntl.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>

namespace pipelang::workspace {
namespace {
std::string stamp(const struct stat& info) {
    return Hashing::text(WorkspacePath::identity(info) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
                         std::to_string(info.st_mtim.tv_nsec) + ":" + std::to_string(info.st_ctim.tv_sec) + ":" +
                         std::to_string(info.st_ctim.tv_nsec));
}
}
DirectoryPage DirectoryPager::list(const std::string& relative, const std::string& after, std::int64_t limit) const {
    if (limit < 1 || limit > 150 || after.size() > 322)
        throw WorkspaceError("limit");
    auto directory = files_.openDirectory(relative);
    struct stat before{};
    if (::fstat(directory.get(), &before) != 0)
        throw WorkspaceError("io_failure");
    auto revision = stamp(before);
    std::string previous;
    if (!after.empty()) {
        if (after.size() < 66 || after.substr(0, 65) != revision + "|")
            throw WorkspaceError("stale_cursor");
        previous = after.substr(65);
        if (WorkspacePath::components(previous).size() != 1)
            throw WorkspaceError("invalid_cursor");
    }
    int duplicate = ::fcntl(directory.get(), F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0)
        throw WorkspaceError("io_failure");
    DIR* stream = ::fdopendir(duplicate);
    if (!stream) {
        ::close(duplicate);
        throw WorkspaceError("io_failure");
    }
    auto closeDirectory = [](DIR* value) { ::closedir(value); };
    std::unique_ptr<DIR, decltype(closeDirectory)> owner(stream, closeDirectory);
    std::vector<DirectoryEntry> entries;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    std::size_t scanned = 0;
    std::size_t pathBytes = 0;
    for (;;) {
        checkCancellation(files_.cancellation());
        errno = 0;
        auto* entry = ::readdir(stream);
        if (!entry) {
            if (errno != 0)
                throw WorkspaceError("io_failure");
            break;
        }
        std::string name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        if (++scanned > 10000 || std::chrono::steady_clock::now() >= deadline)
            throw WorkspaceError("directory_limit");
        try {
            WorkspacePath::components(name);
        } catch (const WorkspaceError&) {
            continue;
        }
        if (name <= previous)
            continue;
        struct stat info{};
        if (::fstatat(directory.get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            throw WorkspaceError("changed");
        if ((!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) || info.st_dev != before.st_dev)
            continue;
        DirectoryEntry selected;
        selected.name = name;
        selected.path = relative.empty() ? name : relative + "/" + name;
        selected.directory = S_ISDIR(info.st_mode);
        selected.identity = WorkspacePath::identity(info);
        entries.push_back(std::move(selected));
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        if (entries.size() > static_cast<std::size_t>(limit) + 1)
            entries.pop_back();
    }
    struct stat afterInfo{};
    if (::fstat(directory.get(), &afterInfo) != 0 || stamp(afterInfo) != revision)
        throw WorkspaceError("stale_cursor");
    DirectoryPage result;
    result.complete = entries.size() <= static_cast<std::size_t>(limit);
    if (!result.complete)
        entries.resize(static_cast<std::size_t>(limit));
    for (const auto& entry : entries) {
        pathBytes += entry.path.size() + entry.name.size() + entry.identity.size();
        if (pathBytes > 65536)
            throw WorkspaceError("limit");
    }
    if (!result.complete)
        result.nextAfter = revision + "|" + entries.back().name;
    result.entries = std::move(entries);
    return result;
}
}
