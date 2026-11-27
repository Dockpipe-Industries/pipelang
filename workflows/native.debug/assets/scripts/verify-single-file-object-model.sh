#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
sdk_tool=$2
compiler_record=$3
llc=$4
gdb=$5
include=$6
proof=$7
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
source="$PWD/tests/native-models/ClassInheritance.pipe"
native_rules="$PWD/sdk/pipeview/model/cmake/PipeLangNativeExecutable.cmake"

mkdir -p "$proof/project"
cat > "$proof/project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
project(PipeLangSingleFileObject LANGUAGES NONE)
include("${PIPELANG_NATIVE_RULES}")

foreach(profile debug release)
    pipelang_add_native_object("inheritance-${profile}"
        MODEL "${PIPELANG_SOURCE}"
        MODEL_EXPORTS App
        ENTRY App.Read
        HOST_SYMBOL proof_single_file_object_model
        HOST_OBJECT_REFERENCES
        HOST_METHODS Identity Reference
        TARGET linux-x64
        PROFILE "${profile}"
        LLC "${PIPELANG_LLC}"
        PROJECT_TOOL "${PIPELANG_SDK_TOOL}"
        LINK_SDK "${PIPELANG_LINK_SDK}"
        LINK_SDK_SHA256 "${PIPELANG_LINK_SDK_SHA256}"
        OUTPUT "native objects/inheritance-${profile}.o")
endforeach()
CMAKE

/usr/bin/env -i PATH=/no-compiler /usr/bin/cmake -S "$proof/project" -B "$proof/build" -G 'Unix Makefiles' \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/make "-DPIPELANG_NATIVE_RULES=$native_rules" "-DPIPELANG_SOURCE=$source" \
    "-DPIPELANG_LLC=$llc" "-DPIPELANG_SDK_TOOL=$sdk_tool" "-DPIPELANG_LINK_SDK=$sdk" \
    "-DPIPELANG_LINK_SDK_SHA256=$(<"$sdk/link-sdk.sha256")"
/usr/bin/env -i PATH=/no-compiler /usr/bin/strace -f -qq -e trace=execve,execveat -o "$proof/emitter.exec" \
    /usr/bin/cmake --build "$proof/build" --parallel 1
if rg -q 'execve\("[^"]*/(clang(\+\+)?|gcc|g\+\+|c\+\+|cc1plus|pipelang-modelc)"' "$proof/emitter.exec"; then
    exit 1
fi

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/SingleFileObjectModel.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/build/native objects/inheritance-$profile.o" \
        "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile"
done

"$gdb" --batch --nx --quiet -ex 'set disable-randomization off' -ex "directory $(dirname "$source")" \
    -ex 'tbreak ClassInheritance.pipe:135' -ex run -ex 'list' -ex continue "$proof/debug" > "$proof/debugger.output" 2>&1
grep -F 'at ClassInheritance.pipe:135' "$proof/debugger.output"
grep -F 'public pure int Read() => alias.Read();' "$proof/debugger.output"
grep -F 'exited normally' "$proof/debugger.output"

published="$proof/build/native objects/inheritance-release.o"
sha256sum "$published" > "$proof/published.sha256"
if "$sdk/pipelang-objectc" --model "$source" --model-export App --entry Base.Read \
    --host-symbol proof_single_file_object_model --llc "$llc" --output "$published" > "$proof/unselected.result" 2>&1; then
    exit 1
fi
grep -F 'native hosted entry class must be selected by --model-export' "$proof/unselected.result"
sha256sum --check --status "$proof/published.sha256"
