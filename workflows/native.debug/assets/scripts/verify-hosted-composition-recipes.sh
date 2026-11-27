#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
source="$assets/HostedCompositionRecipes.pipe"
composition=NativeProof.ScopedPorts.Startup

mkdir "$proof"
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/HostedCompositionRecipes.cpp" -o "$proof/driver.o"
options=(--source "$source" --entry NativeProof.ScopedPorts.Root.Read --llc "$llc"
    --host-symbol proof_root --host-object-references --host-multiple-tasks --host-snapshot
    --host-method Canary --host-method ReadLater --host-method Release --host-method Hold --host-method Next)
for profile in debug release; do
    "$sdk/pipelang-objectc" "${options[@]}" --host-composition "$composition" --profile "$profile" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile"
done

sha256sum "$proof/release.o" > "$proof/published.sha256"
if "$sdk/pipelang-objectc" "${options[@]}" --host-composition NativeProof.ScopedPorts.Absent \
    --profile release --output "$proof/release.o" > "$proof/refusal.output" 2>&1; then
    exit 1
fi
grep -F 'native hosted composition was not found' "$proof/refusal.output"
sha256sum --check --status "$proof/published.sha256"
