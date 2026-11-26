#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

cli=$1
sdk=$2
llc=$3
linker=$4
compiler_path=$5
proof=$6
bindir=${7:-bin}
prefix="$proof/installed SDK"
native_compiler="$prefix/$compiler_path"

mkdir -p "$prefix/$bindir"
install -m 0755 "$cli" "$prefix/$bindir/pipelang"
install -D -m 0755 "$sdk/pipelang-objectc" "$native_compiler"
empty_path=(/usr/bin/env -i PATH=/no-compiler)
command=("${empty_path[@]}" "$prefix/$bindir/pipelang")
"${command[@]}" --help | grep -F 'pipelang build [--backend native]'

args=(--source "$PWD/examples/native/Standalone.pipe" --entry Standalone.App.Run
    --output "$proof/app" --profile release --target linux-x64
    --linker "$linker" --link-sdk "$sdk" --link-sdk-sha256 "$(<"$sdk/link-sdk.sha256")")
/usr/bin/strace -f -qq -e trace=execve,execveat -o "$proof/build.exec" "${command[@]}" build "${args[@]}" --llc "$llc"
test "$("${empty_path[@]}" "$proof/app")" = 42
unexpected_compiler='execve\("[^"]*/(pipelang-modelc|g\+\+|c\+\+|clang(\+\+)?|cc1plus)(-[0-9]+)?"'
if grep -E "$unexpected_compiler" "$proof/build.exec"; then
    exit 1
fi

cp "$proof/app" "$proof/last-good"
"${command[@]}" build --backend native "${args[@]}" --llc "$llc"
cmp "$proof/app" "$proof/last-good"
if "${command[@]}" build --backend unrecognized > "$proof/refused.output" 2>&1; then
    exit 1
fi
grep -F "unsupported executable build backend 'unrecognized'" "$proof/refused.output"

if /usr/bin/strace -f -qq -e trace=execve,execveat -o "$proof/codegen-failure.exec" \
    "${command[@]}" build "${args[@]}" --llc /usr/bin/false > "$proof/codegen-failure.output" 2>&1; then
    exit 1
fi
grep -F 'LLVM object emission failed for linux-x64:' "$proof/codegen-failure.output"
grep -F 'execve("/usr/bin/false"' "$proof/codegen-failure.exec"
# The failed invocation runs env, the CLI, the object compiler, and the selected LLVM step.
test "$(grep -Ec 'execve(at)?\(' "$proof/codegen-failure.exec")" -eq 4
grep -F "execve(\"$native_compiler\"" "$proof/codegen-failure.exec"
cmp "$proof/app" "$proof/last-good"

mv "$native_compiler" "$proof/saved-compiler"
if "${command[@]}" build "${args[@]}" --llc "$llc" > "$proof/missing.output" 2>&1; then
    exit 1
fi
cmp "$proof/app" "$proof/last-good"
grep -F 'pipelang-objectc' "$proof/missing.output"
printf '%s\n' 'The installed CLI built the native executable with no C++ compiler on PATH.'
