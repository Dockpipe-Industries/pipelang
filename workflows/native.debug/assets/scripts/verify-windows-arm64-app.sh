#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
x64_sdk=$2
tools=$3/bin
llc=$4
source=$5
proof=$6
compiler="$sdk/pipelang-objectc"
digest=$(<"$sdk/link-sdk.sha256")
arguments=(--source "$source" --entry SuspendingCollections.App.Run --llc "$llc"
    --target windows-arm64 --executable --linker "$tools/ld.lld")

mkdir "$proof/debug" "$proof/release" "$proof/repeat"
for profile in debug release; do
    app="$proof/$profile/suspending.exe"
    /usr/bin/strace -qq -f -e trace=execve,execveat -o "$proof/$profile/build.exec" \
        "$compiler" "${arguments[@]}" --profile "$profile" --link-sdk "$sdk" --link-sdk-sha256 "$digest" --output "$app"
    test "$(grep -Ec 'execve(at)?\(' "$proof/$profile/build.exec")" -eq 3

    "$tools/llvm-readobj" --file-headers --coff-imports --coff-debug-directory "$app" > "$proof/$profile/image.txt"
    grep -F 'Machine: IMAGE_FILE_MACHINE_ARM64' "$proof/$profile/image.txt"
    grep -F 'PDBFileName: suspending.pdb' "$proof/$profile/image.txt"
    imports=$(awk '$1 == "Name:" {print tolower($2)}' "$proof/$profile/image.txt")
    test -n "$imports"
    if grep -Eiv '^(kernel32|advapi32|shell32|user32|ucrtbase|msvcrt|api-ms-win-crt-[a-z0-9-]+)\.dll$' <<< "$imports"; then
        exit 1
    fi

    "$tools/llvm-pdbutil" dump --files -l "$proof/$profile/suspending.pdb" > "$proof/$profile/lines.txt"
    grep -m 1 -F 'SuspendingCollections.pipe' "$proof/$profile/lines.txt"
done

"$compiler" "${arguments[@]}" --link-sdk "$sdk" --link-sdk-sha256 "$digest" --output "$proof/repeat/suspending.exe"
cmp "$proof/debug/suspending.exe" "$proof/repeat/suspending.exe"
cmp "$proof/debug/suspending.pdb" "$proof/repeat/suspending.pdb"

if "$compiler" "${arguments[@]}" --link-sdk "$x64_sdk" --link-sdk-sha256 "$(<"$x64_sdk/link-sdk.sha256")" \
    --output "$proof/debug/suspending.exe" 2>"$proof/wrong-sdk.stderr"; then
    exit 1
fi
grep -F 'native link SDK manifest omits link/libclang_rt.builtins-aarch64.a' "$proof/wrong-sdk.stderr"
cmp "$proof/debug/suspending.exe" "$proof/repeat/suspending.exe"
cmp "$proof/debug/suspending.pdb" "$proof/repeat/suspending.pdb"
