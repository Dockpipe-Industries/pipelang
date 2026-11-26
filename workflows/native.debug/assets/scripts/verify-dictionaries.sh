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
source="$assets/Dictionaries.pipe"
cxx=$(<"$compiler_record")
mkdir "$proof"

for profile in debug release; do
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" \
        NativeProof.Dictionaries.App.Run "$proof/$profile" "$profile"
    test "$(env -i "$proof/$profile" 0 --require-suspension)" = 42
done

"$sdk/pipelang-objectc" --source "$source" --entry NativeProof.Dictionaries.Session.Run \
    --host-symbol proof_dictionaries --llc "$llc" --output "$proof/hosted.o"
"$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    "$assets/DictionariesHosted.cpp" "$proof/hosted.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/hosted"
env -i "$proof/hosted"
jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    entry: "NativeProof.Dictionaries",
    namespace: "reference",
    modules: [{name: "NativeProof.Dictionaries", path: "Dictionaries.pipe", source: $source}]
}' | "$model_compiler" >"$proof/reference.json"
jq -er 'select(.ok) | .header' "$proof/reference.json" >"$proof/Model.hpp"
jq -er 'select(.ok) | .types_header' "$proof/reference.json" >"$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -pthread -DPIPELANG_NUMERIC_FIXTURE=Dictionaries -I "$sdk_include" -I "$proof" \
    "$assets/UInt32ValuesReference.cpp" -o "$proof/reference"

failures=('dictionary key already exists' 'dictionary key was not found' 'collection owned byte limit exceeded')
for backend in debug release reference; do
    test "$(env -i "$proof/$backend" 0)" = 42
    for index in "${!failures[@]}"; do
        scenario=$((index + 1))
        output="$proof/$backend.$scenario"
        status=0
        env -i "$proof/$backend" "$scenario" >"$output.stdout" 2>"$output.stderr" || status=$?
        test "$status" -eq 1
        test ! -s "$output.stdout"
        grep -Fx "${failures[index]}" "$output.stderr"
    done
done
