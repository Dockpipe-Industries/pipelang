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
source="$assets/FloatingValues.pipe"
mkdir "$proof"

for profile in debug release; do
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" \
        NativeProof.FloatingValues.App.Run "$proof/$profile" "$profile"
    test "$(env -i "$proof/$profile" 0 --require-suspension)" = 42
done

"$sdk/pipelang-objectc" --source "$source" --entry NativeProof.FloatingValues.Session.Run \
    --host-symbol proof_floating_session --llc "$llc" --output "$proof/hosted.o"
"$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    "$assets/FloatingHosted.cpp" "$proof/hosted.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/hosted"
env -i "$proof/hosted"

jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    entry: "NativeProof.FloatingValues",
    namespace: "reference",
    modules: [{name: "NativeProof.FloatingValues", path: "FloatingValues.pipe", source: $source}]
}' | "$model_compiler" >"$proof/reference.json"
jq -er 'select(.ok) | .header' "$proof/reference.json" >"$proof/Model.hpp"
jq -er 'select(.ok) | .types_header' "$proof/reference.json" >"$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -pthread -DPIPELANG_NUMERIC_FIXTURE=FloatingValues -I "$sdk_include" -I "$proof" \
    "$assets/UInt32ValuesReference.cpp" -o "$proof/reference"

errors=(
    'floating-to-int conversion requires a finite value in signed64 range'
    'floating-to-int rounding requires a finite value in signed64 range'
    'floating-to-uint32 conversion requires a finite truncated value in range 0..4294967295'
    'floating clamp requires finite values and ordered bounds'
)
for mode in debug release reference; do
    env -i "$proof/$mode" 0 >"$proof/$mode.success"
    grep -Fx 42 "$proof/$mode.success"
    for index in "${!errors[@]}"; do
        scenario=$((index + 1))
        output="$proof/$mode.$scenario"
        status=0
        env -i "$proof/$mode" "$scenario" >"$output.stdout" 2>"$output.stderr" || status=$?
        test "$status" -eq 1
        test ! -s "$output.stdout"
        printf '%s\n' "${errors[index]}" >"$proof/expected.stderr"
        cmp "$proof/expected.stderr" "$output.stderr"
    done
done
