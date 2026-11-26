// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Target.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace pipelang::models::native_ir {
namespace {

constexpr std::array targets{
    Target{"linux-x64", "x86_64-unknown-linux-gnu", ObjectFormat::Elf, Architecture::X64, ".o",
           "x86_64-unknown-linux-gnu"},
    Target{"linux-arm64", "aarch64-unknown-linux-gnu", ObjectFormat::Elf, Architecture::Arm64, ".o",
           "aarch64-unknown-linux-gnu"},
    // The packaged Windows runtime is linked with LLVM-MinGW's compiler
    // support, including the GNU ABI stack-probe entry point.
    Target{"windows-x64", "x86_64-pc-windows-msvc", ObjectFormat::Coff, Architecture::X64, ".obj",
           "x86_64-w64-windows-gnu"},
    Target{"windows-arm64", "aarch64-pc-windows-msvc", ObjectFormat::Coff, Architecture::Arm64, ".obj",
           "aarch64-w64-windows-gnu"},
    Target{"macos-x64", "x86_64-apple-macosx11.0.0", ObjectFormat::MachO, Architecture::X64, ".o",
           "x86_64-apple-macosx11.0.0"},
    Target{"macos-arm64", "arm64-apple-macosx11.0.0", ObjectFormat::MachO, Architecture::Arm64, ".o",
           "arm64-apple-macosx11.0.0"}
};

std::uint16_t read16(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}

std::uint32_t read32(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(read16(bytes, offset)) |
           (static_cast<std::uint32_t>(read16(bytes, offset + 2)) << 16);
}

}

const Target& findTarget(std::string_view name)
{
    const auto selected = std::find_if(targets.begin(), targets.end(), [name](const Target& target) {
        return target.name == name;
    });

    if (selected == targets.end())
        throw std::invalid_argument("unsupported native target '" + std::string(name) +
            "'; select linux-x64, linux-arm64, windows-x64, windows-arm64, macos-x64 or macos-arm64");

    return *selected;
}

std::string_view hostTargetName()
{
#if defined(__aarch64__) || defined(_M_ARM64)
# if defined(_WIN32)
    return "windows-arm64";
# elif defined(__APPLE__)
    return "macos-arm64";
# elif defined(__linux__)
    return "linux-arm64";
# endif
#elif defined(__x86_64__) || defined(_M_X64)
# if defined(_WIN32)
    return "windows-x64";
# elif defined(__APPLE__)
    return "macos-x64";
# elif defined(__linux__)
    return "linux-x64";
# endif
#endif

    throw std::runtime_error("this compiler host requires an explicit --target");
}

bool matchesObjectTarget(std::string_view object, const Target& target)
{
    const bool arm64 = target.architecture == Architecture::Arm64;
    switch (target.format) {
    case ObjectFormat::Elf:
        // Little-endian ELF64 relocatable object.
        return object.size() >= 64 && object.starts_with("\x7f" "ELF\x02\x01\x01") &&
               read16(object, 16) == 1 && read16(object, 18) == (arm64 ? 183 : 62);
    case ObjectFormat::Coff:
        // Standard COFF header, with sections and no PE optional header.
        return object.size() >= 20 && read16(object, 0) == (arm64 ? 0xaa64 : 0x8664) &&
               read16(object, 2) != 0 && read16(object, 16) == 0 && (read16(object, 18) & 0x0002) == 0;
    case ObjectFormat::MachO:
        // Little-endian Mach-O 64-bit slice, with MH_OBJECT file type.
        return object.size() >= 32 && read32(object, 0) == 0xfeedfacf &&
               read32(object, 4) == (arm64 ? 0x0100000c : 0x01000007) && read32(object, 12) == 1;
    }

    return false;
}

}
