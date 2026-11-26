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
source="$assets/HostedContextOwner.pipe"

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    entry: "NativeProof.HostedContextOwner",
    namespace: "reference",
    modules: [{name: "NativeProof.HostedContextOwner", path: "HostedContextOwner.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -e -r 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -e -r 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"

compile=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
"$cxx" "${compile[@]}" -DPIPELANG_CONTEXT_CPP_REFERENCE -I "$proof" \
    "$assets/HostedContextOwner.cpp" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
"$cxx" "${compile[@]}" -fPIE -c "$assets/HostedContextOwner.cpp" -o "$proof/driver.o"

for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry NativeProof.HostedContextOwner.ContextOwnerModel.Lifetime \
        --host-symbol proof_hosted_context_owner --host-method Replace --host-method Invalidate \
        --host-method InvalidateThenFail --host-method Close --host-method Changes --host-method CreateChild \
        --host-method DropChild --host-method ChildLifetime --host-method CreateIncompleteChild --host-method Exercise \
        --output "$proof/$profile.o" --profile "$profile" --llc "$llc"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"

    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" \
        NativeProof.HostedContextOwner.Entry.Run "$proof/$profile-standalone" "$profile"
    for scenario in 0 1; do
        test "$(env -i "$proof/$profile-standalone" "$scenario")" = 42
    done
done
cat "$proof/reference.output"
