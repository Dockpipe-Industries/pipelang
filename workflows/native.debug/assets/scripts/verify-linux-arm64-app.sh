#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
sdk=$2
x64_sdk=$3
llc=$4
linker=$5
qemu=$6
source=$7
proof=$8
scripts=$(dirname "$0")
llvm=$(dirname "$llc")
digest=$(<"$sdk/link-sdk.sha256")
arguments=(--source "$source" --entry SuspendingCollections.App.Run --llc "$llc"
    --target linux-arm64 --executable --linker "$linker")

for profile in debug release; do
    mkdir -p "$proof/$profile/standalone" "$proof/$profile/repeat"
    app="$proof/$profile/standalone/suspending"
    /usr/bin/strace -qq -f -e trace=execve,execveat -o "$proof/$profile/build.exec" \
        "$compiler" "${arguments[@]}" --profile "$profile" --link-sdk "$sdk" \
        --link-sdk-sha256 "$digest" --output "$app"
    test "$(grep -c 'execve(' "$proof/$profile/build.exec")" -eq 3
    for executable in "$compiler" "$llc" "$linker"; do
        grep -F "execve(\"$executable\"," "$proof/$profile/build.exec" >/dev/null
    done
    "$llvm/llvm-readobj" --file-headers "$app" >"$proof/$profile/headers.txt"
    grep -F 'Machine: EM_AARCH64' "$proof/$profile/headers.txt" >/dev/null
    "$llvm/llvm-dwarfdump" --debug-line "$app" >"$proof/$profile/line-tables.txt"
    grep -F 'SuspendingCollections.pipe' "$proof/$profile/line-tables.txt" >/dev/null
    /bin/bash "$scripts/verify-suspending-app.sh" "$app" "$qemu"
    "$compiler" "${arguments[@]}" --profile "$profile" --link-sdk "$sdk" \
        --link-sdk-sha256 "$digest" --output "$proof/$profile/repeat/suspending"
    cmp "$app" "$proof/$profile/repeat/suspending"
done

app="$proof/debug/standalone/suspending"
if "$compiler" "${arguments[@]}" --link-sdk "$x64_sdk" \
    --link-sdk-sha256 "$(<"$x64_sdk/link-sdk.sha256")" --output "$app" 2>"$proof/wrong-sdk.stderr"; then
    exit 1
fi
grep -F 'native link SDK crt1.o does not match linux-arm64' "$proof/wrong-sdk.stderr"
cmp "$app" "$proof/debug/repeat/suspending"
