#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    -c "$assets/HostedObjectComposition.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source tests/native-models/ObjectComposition.pipe \
        --entry ObjectComposition.App.BagWork --host-symbol proof_object_composition --host-method BagState \
        --host-method FailBagMutation --host-method UpcastEnumeration --host-method PrivateFactory \
        --host-method Text --host-method IncrementField --host-method InternalNames \
        --llc "$llc" --profile "$profile" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
