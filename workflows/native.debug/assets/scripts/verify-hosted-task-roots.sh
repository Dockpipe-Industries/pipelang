#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
sdk_include=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
source="$assets/HostedTaskRoots.pipe"

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    namespace: "reference",
    entry: "NativeProof.HostedTaskRoots",
    modules: [{name: "NativeProof.HostedTaskRoots", path: "HostedTaskRoots.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"

compile=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I "$sdk_include")
"$cxx" "${compile[@]}" -I "$proof" -DPIPELANG_MULTIPLE_TASKS_CPP_REFERENCE "$assets/HostedTaskRoots.cpp" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
"$cxx" "${compile[@]}" -fPIE -c "$assets/HostedTaskRoots.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry NativeProof.HostedTaskRoots.Session.Run \
        --host-symbol proof_hosted_task_roots --host-multiple-tasks \
        --host-method Reset --host-method Drop --host-method Wait --host-method Signal \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"
done
cat "$proof/reference.output"
