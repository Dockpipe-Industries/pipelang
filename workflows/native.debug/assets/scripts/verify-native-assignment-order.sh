#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
include=$4
proof=$5
cxx=$(<"$compiler_record")
fixture="$PWD/tests/native-models/assignment-order"

mkdir "$proof"
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$fixture/NativeAssignmentOrderTests.cpp" -o "$proof/driver.o"

for profile in debug release; do
    "$sdk/pipelang-objectc" --project-root "$fixture" --project-entry Startup.pipe \
        --entry AssignmentOrder.Probe.Run --host-symbol proof_assignment --host-method RunRecord --host-method Ordered \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
