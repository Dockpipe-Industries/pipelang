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

mkdir "$proof"
for fixture in AliasedExports CompositionFactories CompositionCleanup PolymorphicFactory ScopedHandles; do
    "$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror \
        -I "$include" -c "$assets/$fixture.cpp" -o "$proof/$fixture.o"
    for profile in debug release; do
        stem="$proof/$fixture-$profile"
        "$sdk/pipelang-objectc" --source "$assets/$fixture.pipe" --entry "NativeProof.$fixture.Root.Run" \
            --host-symbol proof_root --host-object-references --host-multiple-tasks \
            --host-composition "NativeProof.$fixture.Startup" --profile "$profile" --llc "$llc" --output "$stem.o"
        "$cxx" -pie -pthread "$proof/$fixture.o" "$stem.o" "$sdk/libpipelang_native_app_runtime.a" -o "$stem"
        env -i "$stem"
    done
done
