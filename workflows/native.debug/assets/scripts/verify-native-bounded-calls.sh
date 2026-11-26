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

source="$PWD/tests/native-models/BoundedCalls.pipe"
methods=(Visit Direct Mutual Generic Virtual Siblings Pure Work CleanupDepth CleanupWork Cleaned Construct ConstructedCount
         ParallelRoot ParallelObject HostReentry LocalAsync ObjectAsync AsyncSiblings VirtualAsync AsyncWork)
method_arguments=()
for method in "${methods[@]}"; do
    method_arguments+=(--host-method "$method")
done

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/NativeBoundedCalls.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry BoundedCalls.App.NestedRunner \
        --host-symbol proof_bounded "${method_arguments[@]}" --host-multiple-tasks --host-snapshot \
        --profile "$profile" --llc "$llc" --output "$proof/$profile-bounded.o"
    "$sdk/pipelang-objectc" --source "$source" --entry BoundedCalls.App.Direct \
        --host-symbol proof_plain_bounded --host-method Visit --profile "$profile" --llc "$llc" \
        --output "$proof/$profile-plain.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile-bounded.o" "$proof/$profile-plain.o" \
        "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile"
done
