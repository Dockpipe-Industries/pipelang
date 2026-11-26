#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
fixtures=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
compiler="$sdk/pipelang-objectc"
runtime="$sdk/libpipelang_native_app_runtime.a"

mkdir "$proof"
for module in BaseTypes Types Entry; do
    cp "$fixtures/data/interfaces_source-$module.pipe" "$proof/$module.pipe"
done
jq -n '{
    protocol: "pipelang.native-model-source-set.v1",
    entry: "Entry",
    models: ["Entry.App"],
    modules: [
        {name: "BaseTypes", path: "BaseTypes.pipe"},
        {name: "Types", path: "Types.pipe"},
        {name: "Entry", path: "Entry.pipe"}
    ]
}' > "$proof/modules.json"

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    -c "$assets/HostedInterfaceInheritance.cpp" -o "$proof/driver.o"
methods=()
for name in Values ExactChoice DerivedChoice BaseChoice TransitiveArguments MutateThroughBase \
    FailInheritedMutation Read WaitThroughBase ReleaseSignal; do
    methods+=(--host-method "$name")
done
for profile in debug release; do
    "$compiler" --source "$fixtures/InterfaceInheritance.pipe" --entry InterfaceInheritance.App.Identities \
        --host-symbol proof_interface_inheritance --host-object-references --host-multiple-tasks \
        "${methods[@]}" --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$compiler" --source-set "$proof/modules.json" --entry Entry.App.Read \
        --host-symbol proof_modular_interface_inheritance --host-object-references \
        --profile "$profile" --llc "$llc" --output "$proof/modules-$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$proof/modules-$profile.o" "$runtime" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
