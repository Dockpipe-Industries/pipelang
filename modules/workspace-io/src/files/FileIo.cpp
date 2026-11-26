// SPDX-License-Identifier: Apache-2.0
#include "FileIo.hpp"
#include "../primitives/CooperativeLock.hpp"
#include "../primitives/Hashing.hpp"
#include "../primitives/Uuid.hpp"
#include "../primitives/WorkspacePath.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>

namespace pipelang::workspace {
namespace {
constexpr std::size_t maximumFileBytes = 1024 * 1024;
void openFailure() {
    if (errno == ENOENT)
        throw WorkspaceError("not_found");
    if (errno == ELOOP || errno == ENOTDIR)
        throw WorkspaceError("invalid_path");
    throw WorkspaceError("io_failure");
}
std::string parentOf(const std::string& path) {
    auto position = path.rfind('/');
    return position == std::string::npos ? "" : path.substr(0, position);
}
std::string nameOf(const std::string& path) {
    auto position = path.rfind('/');
    return position == std::string::npos ? path : path.substr(position + 1);
}
void validateRegular(const struct stat& info) {
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != ::geteuid())
        throw WorkspaceError("not_regular");
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > maximumFileBytes)
        throw WorkspaceError("limit");
}
}
FileIo::FileIo(std::string home, Cancellation cancelled)
    : home_(std::move(home)), root_(WorkspacePath::openAbsoluteDirectory(home_)), cancelled_(std::move(cancelled)) {
    if (::fstat(root_.get(), &rootIdentity_) != 0 || rootIdentity_.st_uid != ::geteuid() ||
        (rootIdentity_.st_mode & 077) != 0)
        throw WorkspaceError("insecure_root");
}
void FileIo::verifyRoot() const {
    checkCancellation(cancelled_);
    auto current = WorkspacePath::openAbsoluteDirectory(home_);
    struct stat info{};
    if (::fstat(current.get(), &info) != 0 || !WorkspacePath::sameFile(info, rootIdentity_) ||
        info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
        throw WorkspaceError("root_changed");
}
OwnedFd FileIo::openDirectory(const std::string& relative) const {
    auto components = WorkspacePath::components(relative, true);
    verifyRoot();
    OwnedFd directory(::openat(root_.get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory)
        openFailure();
    for (const auto& component : components) {
        checkCancellation(cancelled_);
        OwnedFd next(::openat(directory.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next)
            openFailure();
        struct stat info{};
        if (::fstat(next.get(), &info) != 0 || info.st_dev != rootIdentity_.st_dev)
            throw WorkspaceError("invalid_path");
        directory = std::move(next);
    }
    return directory;
}
FileData FileIo::readAt(int directory, const std::string& name, struct stat* identity) const {
    struct stat before{};
    if (::fstatat(directory, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0)
        openFailure();
    validateRegular(before);
    OwnedFd file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!file)
        openFailure();
    struct stat opened{};
    if (::fstat(file.get(), &opened) != 0 || !WorkspacePath::unchanged(before, opened))
        throw WorkspaceError("changed");
    FileData result;
    result.bytes.reserve(static_cast<std::size_t>(opened.st_size));
    std::array<std::uint8_t, 65536> buffer{};
    for (;;) {
        checkCancellation(cancelled_);
        auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw WorkspaceError("io_failure");
        if (count == 0)
            break;
        if (result.bytes.size() + static_cast<std::size_t>(count) > maximumFileBytes)
            throw WorkspaceError("limit");
        result.bytes.insert(result.bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    struct stat after{};
    struct stat named{};
    if (::fstat(file.get(), &after) != 0 || ::fstatat(directory, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !WorkspacePath::unchanged(opened, after) || !WorkspacePath::unchanged(after, named) ||
        result.bytes.size() != static_cast<std::size_t>(after.st_size))
        throw WorkspaceError("changed");
    result.hash = Hashing::bytes(result.bytes);
    result.identity = WorkspacePath::identity(after);
    if (identity)
        *identity = after;
    return result;
}
FileData FileIo::read(const std::string& relative) const {
    WorkspacePath::components(relative);
    auto directory = openDirectory(parentOf(relative));
    return readAt(directory.get(), nameOf(relative));
}
void FileIo::writeNew(const std::string& relative, const std::vector<std::uint8_t>& bytes) const {
    publish(relative, bytes, nullptr);
}
void FileIo::replace(const std::string& relative, const std::string& expectedHash,
                     const std::vector<std::uint8_t>& bytes) const {
    if (expectedHash.size() != 64 || !std::all_of(expectedHash.begin(), expectedHash.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    }))
        throw WorkspaceError("invalid_revision");
    publish(relative, bytes, &expectedHash);
}
void FileIo::publish(const std::string& relative, const std::vector<std::uint8_t>& bytes,
                     const std::string* expectedHash) const {
    WorkspacePath::components(relative);
    if (bytes.size() > maximumFileBytes)
        throw WorkspaceError("limit");
    CooperativeLock guard(root_.get(), mutation_, cancelled_);
    auto directory = openDirectory(parentOf(relative));
    auto name = nameOf(relative);
    struct stat previous{};
    if (expectedHash && readAt(directory.get(), name, &previous).hash != *expectedHash)
        throw WorkspaceError("stale_revision");
    auto temporary = ".workspace-save-" + Uuid::create();
    OwnedFd file(::openat(directory.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!file)
        openFailure();
    bool published = false;
    bool temporaryPresent = true;
    struct stat created{};
    if (::fstat(file.get(), &created) != 0) {
        ::unlinkat(directory.get(), temporary.c_str(), 0);
        throw WorkspaceError("io_failure");
    }
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            checkCancellation(cancelled_);
            auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw WorkspaceError("io_failure");
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(file.get()) != 0)
            throw WorkspaceError("io_failure");
        checkCancellation(cancelled_);
        struct stat temporaryInfo{};
        if (::fstatat(directory.get(), temporary.c_str(), &temporaryInfo, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(temporaryInfo.st_mode) || temporaryInfo.st_nlink != 1 ||
            !WorkspacePath::sameFile(created, temporaryInfo))
            throw WorkspaceError("changed");
        if (expectedHash) {
            struct stat current{};
            if (::fstatat(directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
                !WorkspacePath::unchanged(previous, current))
                throw WorkspaceError("stale_revision");
            if (::renameat(directory.get(), temporary.c_str(), directory.get(), name.c_str()) != 0)
                openFailure();
            temporaryPresent = false;
            published = true;
        } else {
            if (::linkat(directory.get(), temporary.c_str(), directory.get(), name.c_str(), 0) != 0) {
                if (errno == EEXIST)
                    throw WorkspaceError("already_exists");
                openFailure();
            }
            published = true;
            if (::unlinkat(directory.get(), temporary.c_str(), 0) != 0)
                throw WorkspaceError("unknown_effect");
            temporaryPresent = false;
        }
        if (::fsync(directory.get()) != 0)
            throw WorkspaceError("unknown_effect");
        if (readAt(directory.get(), name).hash != Hashing::bytes(bytes))
            throw WorkspaceError("unknown_effect");
    } catch (...) {
        bool cleanupFailed = false;
        if (temporaryPresent) {
            struct stat named{};
            if (::fstatat(directory.get(), temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                !WorkspacePath::sameFile(created, named) || ::unlinkat(directory.get(), temporary.c_str(), 0) != 0)
                cleanupFailed = true;
        }
        if (published || cleanupFailed)
            throw WorkspaceError("unknown_effect");
        throw;
    }
}
void FileIo::makeDirectory(const std::string& relative) const {
    auto components = WorkspacePath::components(relative);
    CooperativeLock guard(root_.get(), mutation_, cancelled_);
    verifyRoot();
    OwnedFd directory(::openat(root_.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    bool changed = false;
    try {
        for (const auto& component : components) {
            checkCancellation(cancelled_);
            if (::mkdirat(directory.get(), component.c_str(), 0700) == 0) {
                changed = true;
                if (::fsync(directory.get()) != 0)
                    throw WorkspaceError("unknown_effect");
            } else if (errno != EEXIST) {
                openFailure();
            }
            OwnedFd next(::openat(directory.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!next)
                openFailure();
            struct stat info{};
            if (::fstat(next.get(), &info) != 0 || info.st_dev != rootIdentity_.st_dev)
                throw WorkspaceError("invalid_path");
            directory = std::move(next);
        }
    } catch (...) {
        if (changed)
            throw WorkspaceError("unknown_effect");
        throw;
    }
}
std::string FileIo::resolve(const std::string& relative) const {
    WorkspacePath::components(relative, true);
    if (relative.empty()) {
        verifyRoot();
        return home_;
    }
    auto directory = openDirectory(parentOf(relative));
    auto name = nameOf(relative);
    OwnedFd selected(::openat(directory.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (!selected || ::fstat(selected.get(), &info) != 0)
        openFailure();
    if (S_ISLNK(info.st_mode) || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_dev != rootIdentity_.st_dev)
        throw WorkspaceError("invalid_path");
    if (S_ISREG(info.st_mode))
        validateRegular(info);
    return home_ + "/" + relative;
}
}
