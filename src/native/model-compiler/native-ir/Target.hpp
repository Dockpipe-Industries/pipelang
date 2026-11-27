// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace pipelang::models::native_ir {

enum class ObjectFormat { Elf, Coff, MachO };
enum class Architecture { X64, Arm64 };

struct Target {
    std::string_view name;
    std::string_view triple;
    ObjectFormat format;
    Architecture architecture;
    std::string_view extension;
    // A target has a bundled application ABI when this triple is present.
    std::string_view runtimeTriple = {};
};

const Target& findTarget(std::string_view name);
std::string_view hostTargetName();
bool matchesObjectTarget(std::string_view object, const Target& target);

}
