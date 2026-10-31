// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <array>
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace pipelang::tools {
fs::path executablePath() {
#ifdef _WIN32
    std::array<wchar_t, 32768> buffer{};
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(count != 0 && count < buffer.size(), "cannot identify the running executable");
    return fs::canonical(fs::path(std::wstring(buffer.data(), count)));
#elif defined(__APPLE__)
    std::array<char, 32768> buffer{};
    std::uint32_t capacity = buffer.size();
    require(_NSGetExecutablePath(buffer.data(), &capacity) == 0, "cannot identify the running executable");
    return fs::canonical(buffer.data());
#elif defined(__linux__)
    return fs::canonical("/proc/self/exe");
#else
    throw std::runtime_error("executable identity is unavailable on this platform");
#endif
}
}
