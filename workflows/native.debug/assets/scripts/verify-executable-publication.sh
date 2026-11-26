#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
source=$3
proof=$4
output="$proof/collections-async"
arguments=(--source "$source" --entry CollectionsAsync.App.Run --llc "$llc"
    --target linux-x64 --executable --link-sdk "$sdk")
digest=$(<"$sdk/link-sdk.sha256")
wrong_digest=$(printf '%064d' 0)
before=$(sha256sum "$output")

if "$sdk/pipelang-objectc" "${arguments[@]}" --link-sdk-sha256 "$wrong_digest" \
    --linker /usr/bin/ld --output "$output" 2>"$proof/invalid-sdk.stderr"; then
    exit 1
fi
grep -F 'SDK manifest checksum mismatch' "$proof/invalid-sdk.stderr"

if "$sdk/pipelang-objectc" "${arguments[@]}" --link-sdk-sha256 "$digest" \
    --linker /usr/bin/false --output "$output" 2>"$proof/failed-link.stderr"; then
    exit 1
fi
grep -F 'native executable link failed for linux-x64' "$proof/failed-link.stderr"
test "$(sha256sum "$output")" = "$before"
test -z "$(find "$proof" -maxdepth 1 -name '.pipelang-link-*' -print)"

"$sdk/pipelang-objectc" "${arguments[@]}" --link-sdk-sha256 "$digest" \
    --linker /usr/bin/ld --output "$proof/collections-repeat"
cmp "$output" "$proof/collections-repeat"
