#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

proof=$1
llc=$2
linker_bin=$3
qemu_arm64=$4
llvm_bin=$(dirname "$llc")
assets=$(cd "$(dirname "$0")/.." && pwd -P)
targets="$proof/targets"

compile_fixture() {
    local triple=$1
    local source=$2
    local output=$3
    "$llc" "-mtriple=$triple" -filetype=obj "$assets/$source" -o "$targets/$output"
}

compile_fixture aarch64-unknown-linux-gnu run-checks.ll linux-arm64-checks.o
"$llvm_bin/llvm-mc" -triple=aarch64-unknown-linux-gnu -filetype=obj \
    "$assets/linux-arm64-entry.s" -o "$targets/linux-arm64-entry.o"
"$linker_bin/ld.lld" -o "$targets/linux-arm64-select" \
    "$targets/linux-arm64-entry.o" "$targets/linux-arm64-checks.o" "$targets/linux-arm64.o"
"$qemu_arm64" "$targets/linux-arm64-select"
printf '%s\n' 'Linux ARM64 executable passed under QEMU user mode'

for specification in 'x64 x86_64 i386:x86-64 AMD64' 'arm64 aarch64 arm64 ARM64'; do
    read -r arch llvm_arch import_arch coff_arch <<< "$specification"
    target="windows-$arch"
    triple="$llvm_arch-pc-windows-msvc"

    compile_fixture "$triple" run-checks.ll "$target-checks.obj"
    compile_fixture "$triple" windows-entry.ll "$target-entry.obj"
    "$llvm_bin/llvm-dlltool" -m "$import_arch" -d "$assets/kernel32.def" -l "$targets/$target-kernel32.lib"
    "$linker_bin/lld-link" "/machine:$arch" /nodefaultlib /entry:windows_entry /subsystem:console \
        /debug:full "/pdb:$targets/$target-select.pdb" "/out:$targets/$target-select.exe" \
        "$targets/$target-entry.obj" "$targets/$target-checks.obj" "$targets/$target.obj" "$targets/$target-kernel32.lib"
    "$llvm_bin/llvm-readobj" --file-headers --coff-imports "$targets/$target-select.exe" > "$targets/$target.image"
    grep -F "IMAGE_FILE_MACHINE_$coff_arch" "$targets/$target.image"
    grep -F 'ExitProcess' "$targets/$target.image"
    "$llvm_bin/llvm-pdbutil" dump -l --symbols "$targets/$target-select.pdb" > "$targets/$target.pdb.txt"
    grep -F 'Select.pipe' "$targets/$target.pdb.txt" >/dev/null
    grep -w 'result' "$targets/$target.pdb.txt" >/dev/null
    grep -w 'accepted' "$targets/$target.pdb.txt" >/dev/null
done

for specification in 'x64 x86_64' 'arm64 arm64'; do
    read -r arch llvm_arch <<< "$specification"
    target="macos-$arch"
    triple="$llvm_arch-apple-macosx11.0.0"

    compile_fixture "$triple" run-checks.ll "$target-checks.o"
    compile_fixture "$triple" macos-entry.ll "$target-entry.o"
done
