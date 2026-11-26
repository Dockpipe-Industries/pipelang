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
source="$assets/HostedObjectReferences.pipe"

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    namespace: "reference",
    entry: "NativeProof.HostedObjectReferences",
    modules: [{name: "NativeProof.HostedObjectReferences", path: "HostedObjectReferences.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"

common=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
"$cxx" "${common[@]}" -I "$proof" -DPIPELANG_OBJECT_CPP_REFERENCE "$assets/HostedObjectReferences.cpp" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
"$cxx" "${common[@]}" -fPIE -c "$assets/HostedObjectReferences.cpp" -o "$proof/driver.o"
"$cxx" "${common[@]}" -fPIE -c "$assets/HostedReceiverReclamation.cpp" -o "$proof/reclamation-driver.o"

compiler="$sdk/pipelang-objectc"
runtime="$sdk/libpipelang_native_app_runtime.a"
options=(--source "$source" --entry NativeProof.HostedObjectReferences.Model.Put --host-symbol proof_hosted_references
    --host-method Temporary --host-method Inspect --host-method Current --host-method Repeat --host-method Drop
    --host-method Wait --host-method Signal --llc "$llc")
for profile in debug release; do
    "$compiler" "${options[@]}" --host-object-references --host-multiple-tasks --profile "$profile" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$runtime" -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"

    "$compiler" --source "$assets/HostedContextOwner.pipe" --entry NativeProof.HostedContextOwner.ContextOwnerModel.Lifetime \
        --host-symbol proof_reclamation --host-snapshot --host-multiple-tasks --host-method CreateChild --host-method DropChild \
        --host-method ChildLifetime --host-method Replace --host-method Changes --llc "$llc" --profile "$profile" \
        --output "$proof/reclamation-$profile.o"
    "$cxx" -pie -pthread "$proof/reclamation-driver.o" "$proof/reclamation-$profile.o" "$runtime" -o "$proof/reclamation-$profile"
    env -i "$proof/reclamation-$profile"
done

if "$compiler" "${options[@]}" --output "$proof/refused.o" > "$proof/refused.output" 2>&1; then
    exit 1
fi
rg -q 'invocation-owned references cannot escape|arguments cannot contain invocation-owned references' "$proof/refused.output"
test ! -e "$proof/refused.o"
cat "$proof/reference.output"
