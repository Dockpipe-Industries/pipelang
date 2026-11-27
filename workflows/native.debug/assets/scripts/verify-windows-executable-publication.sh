#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
toolchain=$2
llc=$3
source=$4
proof=$5
app="$proof/standalone/collections-async.exe"
pdb="$proof/collections-async.pdb"
digest=$(<"$sdk/link-sdk.sha256")
arguments=(--source "$source" --entry SuspendingCollections.App.Run --llc "$llc"
    --target windows-x64 --executable --link-sdk "$sdk" --link-sdk-sha256 "$digest")
before=$(sha256sum "$app" "$pdb")

if "$sdk/pipelang-objectc" "${arguments[@]}" --pdb "$pdb" --output "$app" \
    --linker /usr/bin/false 2>"$proof/failed-link.stderr"; then
    exit 1
fi
grep -F 'native executable link failed for windows-x64' "$proof/failed-link.stderr"
test "$(sha256sum "$app" "$pdb")" = "$before"
test -z "$(find "$proof" -maxdepth 2 \( -name '.pipelang-link-*' -o -name '*.pipelang-publish' \) -print)"

mkdir "$proof/publication-check"
output="$proof/publication-check/collections-async.exe"
"$sdk/pipelang-objectc" "${arguments[@]}" --output "$output" --linker "$toolchain/bin/ld.lld"
cmp "$app" "$output"
cmp "$pdb" "$proof/publication-check/collections-async.pdb"
