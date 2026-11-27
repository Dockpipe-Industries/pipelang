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
source="$PWD/tests/native-models/ClassInheritance.pipe"
mkdir "$proof"

/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model.v3",
    namespace: "reference",
    models: ["App"],
    source: $source
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/ClassInheritance.hpp"

common=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
driver="$assets/HostedInheritedEntry.cpp"
"$cxx" "${common[@]}" -I "$proof" -DPIPELANG_INHERITANCE_CPP_REFERENCE "$driver" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
"$cxx" "${common[@]}" -fPIE -c "$driver" -o "$proof/driver.o"

for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry 'ClassInheritance.Deep.Fixed()' --host-symbol proof_inherited \
        --host-method 'Fixed(int)' --host-method ViaThis --host-method Read \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"
done
cat "$proof/reference.output"
