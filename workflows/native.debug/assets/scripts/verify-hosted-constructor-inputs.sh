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

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/ConstructorSharedInputs.cpp" -o "$proof/driver.o"
for profile in debug release; do
    common=(--source "$assets/ConstructorSharedInputs.pipe" --entry NativeProof.ConstructorSharedInputs.Session.Run
            --profile "$profile" --llc "$llc")
    "$sdk/pipelang-objectc" "${common[@]}" --host-symbol proof_constructor_inputs --output "$proof/$profile.o"
    "$sdk/pipelang-objectc" "${common[@]}" --host-symbol proof_constructor_inputs_graph \
        --host-object-references --output "$proof/$profile-graph.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$proof/$profile-graph.o" \
        "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile"
done
