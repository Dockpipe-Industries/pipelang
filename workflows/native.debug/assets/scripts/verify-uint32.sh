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
source="$assets/UInt32Values.pipe"
mkdir "$proof"

for profile in debug release; do
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" \
        NativeProof.UInt32Values.App.Run "$proof/$profile" "$profile"
    test "$(env -i "$proof/$profile" 0 --require-suspension)" = 4294967295
done

build_reference() {
    local input=$1
    local destination=$2
    jq -n --rawfile source "$input" --arg path "$(basename "$input")" '{
        protocol: "pipelang.native-model-project.v1",
        entry: "NativeProof.UInt32Values",
        namespace: "reference",
        modules: [{name: "NativeProof.UInt32Values", path: $path, source: $source}]
    }' | "$model_compiler" >"$destination/reference.json"
    jq -er 'select(.ok) | .header' "$destination/reference.json" >"$destination/Model.hpp"
    jq -er 'select(.ok) | .types_header' "$destination/reference.json" >"$destination/Types.hpp"
    "$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" -I "$destination" \
        "$assets/UInt32ValuesReference.cpp" -o "$destination/reference"
}
build_reference "$source" "$proof"

errors=(
    'uint32 addition overflow'
    'uint32 subtraction underflow'
    'uint32 multiplication overflow'
    'uint32 division by zero'
    'uint32 remainder by zero'
    'uint32 shift count must be between 0 and 31'
    'uint32 shift count must be between 0 and 31'
    'uint32 conversion requires an int in range 0..4294967295'
    'uint32 conversion requires an int in range 0..4294967295'
)
for mode in debug release reference; do
    env -i "$proof/$mode" 0 >"$proof/$mode.success"
    grep -Fx 4294967295 "$proof/$mode.success"
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

"$sdk/pipelang-objectc" --source "$source" --entry NativeProof.UInt32Values.Session.Run \
    --host-symbol proof_uint32_session --llc "$llc" --output "$proof/hosted.o"
"$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    "$assets/UInt32Hosted.cpp" "$proof/hosted.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/hosted"
env -i "$proof/hosted"

generated="$proof/generated"
mkdir "$generated"
/bin/bash "$assets/scripts/generate-uint32-cases.sh" "$generated/Generated.pipe" "$generated/expected.txt"
build_reference "$generated/Generated.pipe" "$generated"
for profile in debug release; do
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$generated/Generated.pipe" \
        NativeProof.UInt32Values.App.Run "$generated/$profile" "$profile"
done
for mode in debug release reference; do
    index=0
    while IFS= read -r _; do
        env -i "$generated/$mode" "$index"
        index=$((index + 1))
    done <"$generated/expected.txt" >"$generated/$mode.txt"
    cmp "$generated/expected.txt" "$generated/$mode.txt"
done
printf '%s\n' '24 seeded uint32 arithmetic sequences matched the independent oracle in all three modes.'
