#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

package=$1
sdk=$2
llc=$3
proof=$4
assets=$(cd "$(dirname "$0")/.." && pwd -P)
source="$proof/project"
build="$proof/build"
staged_sdk="$proof/link SDK"
empty_path=(/usr/bin/env -i PATH=/no-compiler)
mkdir -p "$source"
cp -a "$sdk" "$staged_sdk"
cp "$assets/HostedContextOwner.pipe" "$source/"
cat > "$source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21.1)
project(NativeObjectProof LANGUAGES NONE)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED COMPONENTS NativeObject)
pipelang_add_native_object(context SOURCE HostedContextOwner.pipe
    ENTRY NativeProof.HostedContextOwner.ContextOwnerModel.Lifetime
    HOST_SYMBOL proof_context HOST_CONSTRUCTOR "()" HOST_METHODS Replace Changes
    HOST_SNAPSHOT HOST_MULTIPLE_TASKS HOST_OBJECT_REFERENCES
    OUTPUT "compiled objects/context.o"
    TARGET linux-x64 PROFILE release LLC "${LLC}"
    LINK_SDK "${LINK_SDK}" LINK_SDK_SHA256 "${LINK_SDK_SHA256}")
get_target_property(generated_object context PIPELANG_NATIVE_OBJECT)
if(NOT generated_object STREQUAL "${CMAKE_CURRENT_BINARY_DIR}/compiled objects/context.o")
    message(FATAL_ERROR "The native object target must publish its object path")
endif()
CMAKE

"${empty_path[@]}" /usr/bin/cmake -S "$source" -B "$build" -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
    "-DPipeLangModel_DIR=$package" "-DLLC=$llc" "-DLINK_SDK=$staged_sdk" \
    "-DLINK_SDK_SHA256=$(<"$staged_sdk/link-sdk.sha256")"
build_with_trace() {
    /usr/bin/strace -f -qq -e trace=execve,execveat -o "$proof/$1.exec" \
        "${empty_path[@]}" /usr/bin/cmake --build "$build" --parallel 1
}

object="$build/compiled objects/context.o"
build_with_trace initial
test -s "$object"
"$(dirname "$llc")/llvm-readobj" --symbols "$object" > "$proof/symbols"
rg -q 'Name: proof_context' "$proof/symbols"
rg -q 'Name: NativeProof.HostedContextOwner.ContextOwnerModel.Replace' "$proof/symbols"
rg -q '"--host-snapshot"' "$proof/initial.exec"
rg -q '"--host-multiple-tasks"' "$proof/initial.exec"
rg -Fq '"--host-object-references"' "$proof/initial.exec"
rg -Fq '"--host-constructor", "()"' "$proof/initial.exec"
if rg -q 'execve\("[^"]*/(g\+\+|c\+\+|clang(\+\+)?|cc1plus)' "$proof/initial.exec"; then
    exit 1
fi
build_with_trace unchanged
if rg -q 'execve\("[^"]*/pipelang-objectc"' "$proof/unchanged.exec"; then
    exit 1
fi

cp "$object" "$proof/last-good.o"
printf '\n' >> "$staged_sdk/pipelang-objectc"
if build_with_trace tampered > "$proof/tampered.output" 2>&1; then
    exit 1
fi
rg -q 'installed SDK content differs from its pinned manifest' "$proof/tampered.output"
if rg -q 'execve\("[^"]*/pipelang-objectc"' "$proof/tampered.exec"; then
    exit 1
fi
cmp "$object" "$proof/last-good.o"
printf '%s\n' 'Installed CMake built a hosted PipeLang object and verified the pinned SDK before invoking the compiler.'
