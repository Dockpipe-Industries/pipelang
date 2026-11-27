// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#include "../../src/native/support/support.hpp"

namespace pipelang::tests {
inline void createDirectoryJunction(const tools::fs::path& link, const tools::fs::path& target) {
    using namespace tools;
    auto destination = fs::canonical(target);
    destination.make_preferred();
    const auto printName = destination.wstring();
    const auto substituteName = std::wstring(L"\\??\\") + printName;
    struct MountPointData {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t names[1];
    };
    const auto substituteBytes = substituteName.size() * sizeof(wchar_t);
    const auto printBytes = printName.size() * sizeof(wchar_t);
    const auto size = offsetof(MountPointData, names) + substituteBytes + printBytes + 2 * sizeof(wchar_t);
    require(size <= MAXIMUM_REPARSE_DATA_BUFFER_SIZE, "junction fixture path is too long");
    std::vector<std::uint64_t> storage((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* data = reinterpret_cast<MountPointData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(size - 8);
    data->substituteLength = static_cast<WORD>(substituteBytes);
    data->printOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    data->printLength = static_cast<WORD>(printBytes);
    std::memcpy(data->names, substituteName.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(data->names) + data->printOffset,
        printName.c_str(), printBytes + sizeof(wchar_t));
    require(fs::create_directory(link), "junction fixture output must be fresh");
    const HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "cannot open junction fixture");
    DWORD returned = 0;
    const bool created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, data,
        static_cast<DWORD>(size), nullptr, 0, &returned, nullptr) != FALSE;
    const auto error = GetLastError();
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool identified = created && GetFileInformationByHandleEx(directory, FileAttributeTagInfo,
        &attributes, sizeof(attributes)) && attributes.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(directory);
    require(created, "cannot create junction fixture (Win32 " + std::to_string(error) + ")");
    if (!identified) {
        RemoveDirectoryW(link.c_str());
        throw std::runtime_error("junction fixture has incorrect native reparse metadata");
    }
}
inline void removeDirectoryJunction(const tools::fs::path& link) {
    tools::require(RemoveDirectoryW(link.c_str()), "cannot remove junction fixture");
}
class DirectoryJunction {
public:
    DirectoryJunction(const tools::fs::path& link, const tools::fs::path& target) : link_(link) {
        createDirectoryJunction(link_, target);
    }
    ~DirectoryJunction() {
        if (active_)
            RemoveDirectoryW(link_.c_str());
    }
    DirectoryJunction(const DirectoryJunction&) = delete;
    DirectoryJunction& operator=(const DirectoryJunction&) = delete;
    void remove() {
        removeDirectoryJunction(link_);
        active_ = false;
    }
private:
    tools::fs::path link_;
    bool active_ = true;
};
}
#endif
