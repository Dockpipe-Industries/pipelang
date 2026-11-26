// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace pipelang::tools {
namespace {
class File {
public:
    explicit File(HANDLE value) : value_(value) {
        require(value != INVALID_HANDLE_VALUE, "cannot open native Windows output");
    }
    ~File() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE get() const { return value_; }
    void close() {
        const auto selected = std::exchange(value_, INVALID_HANDLE_VALUE);
        require(CloseHandle(selected), "cannot close native Windows output");
    }
private:
    HANDLE value_;
};
class PrivateAccess {
public:
    PrivateAccess() {
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "cannot identify private output owner");
        DWORD capacity = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &capacity);
        if (capacity == 0) {
            CloseHandle(token);
            throw std::runtime_error("cannot size private output owner");
        }
        owner_.resize(capacity);
        const bool identified = GetTokenInformation(token, TokenUser, owner_.data(), capacity, &capacity) != FALSE;
        CloseHandle(token);
        require(identified, "cannot read private output owner");
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid());
        require(SetEntriesInAclW(1, &entry, nullptr, &acl_) == ERROR_SUCCESS, "cannot create private output access list");
        const bool initialized = InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) &&
            SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) &&
            SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
        if (!initialized) {
            LocalFree(acl_);
            acl_ = nullptr;
            throw std::runtime_error("cannot initialize private output security");
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
    }
    ~PrivateAccess() { LocalFree(acl_); }
    PrivateAccess(const PrivateAccess&) = delete;
    PrivateAccess& operator=(const PrivateAccess&) = delete;
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(owner_.data())->User.Sid; }
    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
private:
    std::vector<unsigned char> owner_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};
void ordinary(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    require(GetFileType(file) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        !(attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)),
        "Windows output must be an ordinary nonsymbolic file");
}
}
void atomicWriteWindows(const fs::path& temporary, const fs::path& output, std::string_view bytes) {
    File file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    ordinary(file.get());
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto capacity = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1U << 20));
        DWORD count = 0;
        require(WriteFile(file.get(), bytes.data() + written, capacity, &count, nullptr) && count > 0,
            "cannot write staged Windows output");
        written += count;
    }
    require(FlushFileBuffers(file.get()), "cannot synchronize staged Windows output");
    file.close();
    require(MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        "cannot publish staged Windows output");
}
bool windowsReparsePoint(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return false;
        throw std::runtime_error("cannot inspect Windows path (Win32 " + std::to_string(error) + "): " + path.string());
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
bool windowsPrivateFile(const fs::path& path) {
    File file(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    ordinary(file.get());
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(GetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS,
        "cannot inspect private output access list");
    try {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        require(GetSecurityDescriptorControl(descriptor, &control, &revision), "cannot inspect private output inheritance");
        bool valid = acl && acl->AceCount == 1 && (control & SE_DACL_PROTECTED);
        if (valid) {
            void* entry = nullptr;
            require(GetAce(acl, 0, &entry), "cannot inspect private output access entry");
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
            PrivateAccess current;
            valid = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags & INHERITED_ACE) &&
                EqualSid(const_cast<DWORD*>(&ace->SidStart), current.sid()) &&
                (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
        }
        LocalFree(descriptor);
        return valid;
    } catch (...) {
        LocalFree(descriptor);
        throw;
    }
}
void appendWindows(const fs::path& path, std::string_view bytes, std::uint64_t maximum) {
    require(bytes.size() <= std::numeric_limits<DWORD>::max(), "Windows append exceeds its line capacity");
    PrivateAccess access;
    File file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, access.attributes(), OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    ordinary(file.get());
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= maximum,
        "Windows append target exceeds its capacity");
    OVERLAPPED position{};
    position.Offset = MAXDWORD;
    position.OffsetHigh = MAXDWORD;
    DWORD count = 0;
    require(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, &position) && count == bytes.size(),
        "cannot append Windows operation event");
    require(FlushFileBuffers(file.get()), "cannot synchronize Windows operation event");
    file.close();
}
}
