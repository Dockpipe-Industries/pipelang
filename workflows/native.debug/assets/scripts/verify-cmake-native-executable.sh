#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
package=$1
sdk=$2
llvm=$3
linker=$4
proof=$5
sdk_tool=$6

mkdir "$proof"
link_sdk="$proof/link SDK"
mkdir "$link_sdk"
mapfile -t link_inputs < <(jq -r '.files[].path | select(. != "pipelang-objectc")' "$sdk/link-sdk.json")
for input in "${link_inputs[@]}"; do
    install -D -m 0644 "$sdk/$input" "$link_sdk/$input"
done
"$sdk_tool" manifest --prefix "$link_sdk" --manifest "$link_sdk/link-sdk.json" --roots "${link_inputs[@]}"
link_digest=$(sha256sum "$link_sdk/link-sdk.json" | cut -d ' ' -f 1)
source="$proof/work space"
build="$proof/build space"
mkdir -p "$source/src" "$source/library/src"
cp examples/native/Standalone.pipe "$source/Standalone.pipe"
cat > "$source/src/Entry.pipe" <<'PIPE'
module NativeCmakeProof;
import NativeCmakeProof.Helper;

public Class AppBase {
    public AppBase() {}

    public static int Run() {
        return Helper.Answer() + 1;
    }
}

public Class App : AppBase {
    public App() {}
}
PIPE
cat > "$source/src/Helper.pipe" <<'PIPE'
module NativeCmakeProof.Helper;
import NativeCmakeProof.SDK;

public model Class Helper {
    public pure int Answer() {
        return SdkAnswer();
    }

    private pure int SdkAnswer() {
        return Answers.Initial();
    }
}
PIPE
cat > "$source/library/src/Answers.pipe" <<'PIPE'
module NativeCmakeProof.SDK;

public Class Answers {
    public static pure int Initial() {
        return 41;
    }
}
PIPE
pin_sources() {
    local source_hash library_hash
    source_hash=$(sha256sum "$source/library/src/Answers.pipe" | cut -d ' ' -f 1)
    jq -n --arg digest "$source_hash" '{
        protocol: "pipelang.source-library.v1", name: "NativeCmakeProof.SDK", version: "0.1.0", source_root: "src",
        sources: [{path: "Answers.pipe", sha256: $digest}]
    }' > "$source/library/library.json"
    library_hash=$(sha256sum "$source/library/library.json" | cut -d ' ' -f 1)
    jq -n --arg digest "$library_hash" --arg root "$source/library" '{
        protocol: "pipelang.source-library-references.v1",
        libraries: [{root: $root, descriptor: "library.json", sha256: $digest}]
    }' > "$source/references.json"
}
pin_sources
cat > "$source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21.1)
project(NativeProof LANGUAGES NONE)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED COMPONENTS NativeExecutable)
set(native_options TARGET linux-x64 LLC "${LLC}" LINKER "${LINKER}"
    LINK_SDK "${LINK_SDK}" LINK_SDK_SHA256 "${LINK_SDK_SHA256}")
pipelang_add_native_executable(single SOURCE Standalone.pipe ENTRY Standalone.App.Run
    OUTPUT bin/standalone ${native_options})
pipelang_add_native_executable(project_app PROJECT_ROOT . PROJECT_ENTRY Entry.pipe
    SOURCE_LIBRARIES references.json
    ENTRY NativeCmakeProof.App.Run OUTPUT bin/project_app PROFILE release ${native_options})
CMAKE
empty_path=(/usr/bin/env -i PATH=/no-compiler)
"${empty_path[@]}" /usr/bin/cmake -S "$source" -B "$build" -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
    "-DPipeLangModel_DIR=$package" "-DLLC=$llvm" "-DLINKER=$linker" "-DLINK_SDK=$link_sdk" \
    "-DLINK_SDK_SHA256=$link_digest"
build_with_trace() {
    local trace=$1
    /usr/bin/strace -f -qq -e execve,execveat -o "$proof/$trace.exec" \
        "${empty_path[@]}" /usr/bin/cmake --build "$build" --parallel 1
}
build_with_trace initial
test "$("${empty_path[@]}" "$build/bin/standalone")" = 42
test "$("${empty_path[@]}" "$build/bin/project_app")" = 42
rg -q 'execve\("[^"]*/native-application/pipelang-objectc"' "$proof/initial.exec"
if rg 'execve\("[^"]*/(g\+\+|c\+\+|clang(\+\+)?|cc1plus)' "$proof/initial.exec"; then
    exit 1
fi
build_with_trace unchanged
if rg -F 'pipelang-objectc",' "$proof/unchanged.exec"; then
    exit 1
fi
sed -i 's/return 41;/return 42;/' "$source/library/src/Answers.pipe"
pin_sources
build_with_trace library_change
test "$("${empty_path[@]}" "$build/bin/project_app")" = 43
test "$(rg -c 'pipelang-objectc", .*--project-root' "$proof/library_change.exec")" = 1
sed -i 's/return Helper.Answer() + 1;/return Helper.Answer() + 2;/' "$source/src/Entry.pipe"
build_with_trace project_change
test "$("${empty_path[@]}" "$build/bin/project_app")" = 44

mkdir "$source/src/moved"
mv "$source/src/Helper.pipe" "$source/src/moved/Helper.pipe"
build_with_trace moved_module
test "$("${empty_path[@]}" "$build/bin/project_app")" = 44
test "$(rg -c 'pipelang-objectc", .*--project-root' "$proof/moved_module.exec")" = 1
/usr/bin/readelf --debug-dump=info "$build/bin/project_app" > "$proof/moved_module.dwarf"
grep -F 'project/moved/Helper.pipe' "$proof/moved_module.dwarf"

cp "$build/bin/project_app" "$proof/last-good"
mv "$source/src/moved/Helper.pipe" "$proof/removed-helper.pipe"
if build_with_trace missing_module > "$proof/missing-module.log" 2>&1; then
    exit 1
fi
grep -F 'PipeLang native project admission failed' "$proof/missing-module.log"
grep -F 'imported project module: NativeCmakeProof.Helper' "$proof/missing-module.log"
cmp "$proof/last-good" "$build/bin/project_app"

mv "$proof/removed-helper.pipe" "$source/src/moved/Helper.pipe"
build_with_trace restored_module
cmp "$proof/last-good" "$build/bin/project_app"
test "$("${empty_path[@]}" "$build/bin/project_app")" = 44

"${empty_path[@]}" /usr/bin/cmake -S "$source" -B "$build" -DLLC=/usr/bin/false
cmp "$proof/last-good" "$build/bin/project_app"
if build_with_trace failed_codegen > "$proof/failed_codegen.log" 2>&1; then
    exit 1
fi
grep -F 'LLVM object emission failed for linux-x64' "$proof/failed_codegen.log"
cmp "$proof/last-good" "$build/bin/project_app"

"${empty_path[@]}" /usr/bin/cmake -S "$source" -B "$build" "-DLLC=$llvm"
build_with_trace restored_codegen
mv "$build/bin/project_app" "$proof/replaced-output"
build_with_trace missing_output
test "$("${empty_path[@]}" "$build/bin/project_app")" = 44
cmp "$proof/replaced-output" "$build/bin/project_app"

sed -i 's/Helper.Answer()/Helper.Unknown()/' "$source/src/Entry.pipe"
if build_with_trace failed; then
    exit 1
fi
cmp "$proof/last-good" "$build/bin/project_app"
test "$("${empty_path[@]}" "$build/bin/project_app")" = 44
printf '%s\n' 'CMake built standalone PipeLang and tracked project edits, module membership and pinned SDK changes without a C++ compiler.'
