#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
include=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
source="$assets/HostedTaskGroups.pipe"

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    namespace: "reference",
    entry: "NativeProof.HostedGroups",
    modules: [{name: "NativeProof.HostedGroups", path: "HostedTaskGroups.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"

compile=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I "$include")
"$cxx" "${compile[@]}" -I "$proof" "$assets/HostedTaskGroupsReference.cpp" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
printf '%s\n' 'host,101,202,303' 'published,202' > "$proof/expected.output"
cmp "$proof/expected.output" "$proof/reference.output"

"$cxx" "${compile[@]}" -fPIE -c "$assets/HostedTaskGroups.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry NativeProof.HostedGroups.Session.Wait \
        --host-symbol proof_hosted_groups --host-multiple-tasks \
        --host-method Release --host-method Settled --host-method Settlement \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"
done

cat "$proof/reference.output"
