#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
runtime=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"
for fixture in HostedCompositionGraph HostedCompositionScopes; do
    "$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror \
        -I "$runtime/include" -I "$runtime/src" -c "$assets/$fixture.cpp" -o "$proof/$fixture.o"
done

options=(--source "$assets/HostedCompositionGraph.pipe" --entry NativeProof.ScopedPorts.Root.Read --llc "$llc"
    --host-symbol proof_root --host-object-references --host-multiple-tasks
    --host-method Next --host-method ReadLater --host-method Release
    --host-composition NativeProof.ScopedPorts.Startup)
for profile in debug release; do
    "$sdk/pipelang-objectc" "${options[@]}" --profile "$profile" --output "$proof/$profile.o"
    for fixture in HostedCompositionGraph HostedCompositionScopes; do
        "$cxx" -pie -pthread "$proof/$fixture.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
            -o "$proof/$profile-$fixture"
        env -i "$proof/$profile-$fixture"
    done
done
