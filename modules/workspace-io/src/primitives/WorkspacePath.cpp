// SPDX-License-Identifier: Apache-2.0
#include "WorkspacePath.hpp"
#include "WorkspaceError.hpp"
#include <pipelang/model/byte_buffer.hpp>
#include <fcntl.h>
#include <algorithm>

namespace pipelang::workspace {
void WorkspacePath::validateText(const std::string& text) {
    try {
        (void)pipelang::model::ByteBuffer::FromUtf8(text);
    } catch (const pipelang::model::ExecutionFailure&) {
        throw WorkspaceError("invalid_path");
    }
}
std::vector<std::string> WorkspacePath::components(const std::string& relative, bool allowRoot) {
    if (relative.empty() && allowRoot)
        return {};
    if (relative.empty() || relative.size() > 4096 || relative.front() == '/' ||
        relative.find('\0') != std::string::npos || relative.find('\\') != std::string::npos)
        throw WorkspaceError("invalid_path");
    validateText(relative);
    std::vector<std::string> result;
    std::size_t position = 0;
    while (position <= relative.size()) {
        auto end = relative.find('/', position);
        auto component = relative.substr(position, end == std::string::npos ? end : end - position);
        std::string folded = component;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32) : static_cast<char>(value);
        });
        if (component.empty() || component == "." || component == ".." || folded == ".git" || component.size() > 255)
            throw WorkspaceError("invalid_path");
        result.push_back(component);
        if (end == std::string::npos)
            break;
        position = end + 1;
    }
    return result;
}
OwnedFd WorkspacePath::openAbsoluteDirectory(const std::string& absolute) {
    if (absolute.empty() || absolute.front() != '/' || absolute.size() > 4096 || absolute.find('\0') != std::string::npos)
        throw WorkspaceError("invalid_root");
    validateText(absolute);
    OwnedFd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    std::size_t position = 1;
    while (position < absolute.size()) {
        auto end = absolute.find('/', position);
        auto component = absolute.substr(position, end == std::string::npos ? end : end - position);
        if (component.empty() || component == "." || component == "..")
            throw WorkspaceError("invalid_root");
        OwnedFd next(::openat(current.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next)
            throw WorkspaceError("invalid_root");
        current = std::move(next);
        if (end == std::string::npos)
            break;
        position = end + 1;
    }
    return current;
}
std::string WorkspacePath::identity(const struct stat& info) {
    return "unix:" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
}
bool WorkspacePath::sameFile(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
bool WorkspacePath::unchanged(const struct stat& left, const struct stat& right) {
    return sameFile(left, right) && left.st_size == right.st_size &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}
}
