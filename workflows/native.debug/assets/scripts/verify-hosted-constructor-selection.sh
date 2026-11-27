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

mkdir "$proof"
source="$proof/Composition.pipe"
cp "$PWD/tests/native-models/data/composition-source.pipe" "$source"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model.v1",
    namespace: "reference",
    source: $source
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Composition.hpp"

common=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
driver="$assets/HostedConstructorSelection.cpp"
"$cxx" "${common[@]}" -I "$proof" -DPIPELANG_COMPOSITION_CPP_REFERENCE "$driver" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"
"$cxx" "${common[@]}" -fPIE -c "$driver" -o "$proof/driver.o"

compiler="$sdk/pipelang-objectc"
options=(--source "$source" --entry Composition.Composition.Evaluate --host-method Floating
    --host-method IndependentDefaults --host-method 'Add(int)' --host-method 'Add( float )'
    --host-snapshot --llc "$llc")
for profile in debug release; do
    "$compiler" "${options[@]}" --profile "$profile" --host-symbol proof_composition_default \
        --host-constructor '()' --output "$proof/$profile-default.o"
    "$compiler" "${options[@]}" --profile "$profile" --host-symbol proof_composition_initial \
        --host-constructor '( int )' --output "$proof/$profile-initial.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile-default.o" "$proof/$profile-initial.o" \
        "$sdk/libpipelang_native_app_runtime.a" -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"
done
if "$compiler" "${options[@]}" --host-symbol proof_ambiguous --output "$proof/rejected.o" > "$proof/ambiguous.output" 2>&1; then
    exit 1
fi
rg -q 'multiple public constructors; select one with --host-constructor' "$proof/ambiguous.output"
if "$compiler" "${options[@]}" --host-symbol proof_missing --host-constructor '(string)' \
        --output "$proof/rejected.o" > "$proof/missing.output" 2>&1; then
    exit 1
fi
rg -Fq 'has no public constructor with parameter types (string)' "$proof/missing.output"
if "$compiler" "${options[@]}" --host-symbol proof_duplicate --host-constructor '()' --host-method 'Add( int )' \
        --output "$proof/rejected.o" > "$proof/duplicate.output" 2>&1; then
    exit 1
fi
rg -Fq 'native hosted method was selected more than once' "$proof/duplicate.output"
test ! -e "$proof/rejected.o"
cat "$proof/reference.output"
