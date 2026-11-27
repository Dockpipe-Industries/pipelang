#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
proof=$2
llc=$3
llvm_bin=$(dirname "$llc")
assets=$(cd "$(dirname "$0")/.." && pwd -P)
mkdir "$proof/targets"
fixtures=(FixedArrayClaims ArrayValues)
symbols=(proof_fixed_array_claims proof_array_values)
for fixture in "${fixtures[@]}"; do
    cp "$assets/$fixture.pipe" "$proof/repeat/$fixture.pipe"
done

for specification in \
    'linux-x64 .o elf64-x86-64' \
    'linux-arm64 .o elf64-littleaarch64' \
    'windows-x64 .obj COFF-x86-64' \
    'windows-arm64 .obj COFF-ARM64' \
    'macos-x64 .o Mach-O 64-bit x86-64' \
    'macos-arm64 .o Mach-O arm64'; do
    read -r target extension format <<< "$specification"
    output="$proof/targets/$target$extension"
    args=(--target "$target" --entry Select.Selection.Select --llc "$llc")

    "$compiler" "${args[@]}" --source "$proof/Select.pipe" --output "$output"
    (cd "$proof/repeat" && "$compiler" "${args[@]}" --source Select.pipe --output "$proof/targets/$target.repeat$extension")
    cmp "$output" "$proof/targets/$target.repeat$extension"

    "$llvm_bin/llvm-readobj" --file-headers --symbols "$output" > "$proof/targets/$target.headers"
    grep -Fx "Format: $format" "$proof/targets/$target.headers"
    grep -F 'Select.Selection.Select' "$proof/targets/$target.headers" >/dev/null
    if [[ "$target" = windows-* ]]; then
        "$llvm_bin/llvm-readobj" --codeview "$output" > "$proof/targets/$target.debug"
        grep -Fi 'pipelang-source' "$proof/targets/$target.debug" >/dev/null
    else
        "$llvm_bin/llvm-dwarfdump" --debug-info "$output" > "$proof/targets/$target.debug"
    fi
    grep -F 'Select.pipe' "$proof/targets/$target.debug" >/dev/null
    grep -w 'result' "$proof/targets/$target.debug" >/dev/null
    grep -w 'accepted' "$proof/targets/$target.debug" >/dev/null

    for index in "${!fixtures[@]}"; do
        fixture=${fixtures[index]}
        host_options=()
        if [[ $fixture == FixedArrayClaims ]]; then
            host_options=(--host-snapshot)
        fi
        for profile in debug release; do
            witness="$proof/targets/$target-$fixture-$profile"
            args=(--target "$target" --entry "NativeProof.$fixture.Session.Run"
                  --host-symbol "${symbols[index]}" --profile "$profile" --llc "$llc" "${host_options[@]}")
            "$compiler" "${args[@]}" --source "$assets/$fixture.pipe" --output "$witness$extension"
            (cd "$proof/repeat" && "$compiler" "${args[@]}" --source "$fixture.pipe" \
                --output "$witness.repeat$extension")
            cmp "$witness$extension" "$witness.repeat$extension"
        done
    done
done

cmp "$proof/targets/linux-x64.o" "$proof/select.o"
