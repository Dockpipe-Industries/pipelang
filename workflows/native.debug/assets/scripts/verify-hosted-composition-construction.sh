#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
sdk_include=$5
proof=$6
debugger=$7
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
project="$proof/project"
fixture="$PWD/tests/native-di/composition-fixture/src"
mkdir -p "$project/src"
cp -R "$fixture/Contracts" "$fixture/Root" "$project/src/"
cp "$assets/HostedCompositionConstruction.pipe" "$project/src/Witness.pipe"

find "$project/src" -type f -name '*.pipe' -print0 | sort -z |
    while IFS= read -r -d '' source; do
        jq -cn --arg path "${source#"$project/src/"}" --rawfile source "$source" '{
            name: ($source | capture("(?m)^module (?<name>[A-Za-z0-9_.]+);").name),
            path: $path, source: $source
        }'
    done | jq -s '{
        protocol: "pipelang.native-model-project.v1",
        entry: "DiFixture.NativeConstruction",
        namespace: "reference",
        modules: .
    }' > "$proof/project.json"

"$model_compiler" < "$proof/project.json" > "$proof/reference.json"
if ! jq -er 'select(.ok == true) | .header' "$proof/reference.json" > "$proof/Model.hpp"; then
    jq '.diagnostics' "$proof/reference.json" >&2
    exit 1
fi
jq -er 'select(.ok == true) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
common=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
driver="$assets/HostedCompositionConstruction.cpp"
"$cxx" "${common[@]}" -DPIPELANG_REFERENCE -I "$proof" "$driver" -o "$proof/reference"
env -i "$proof/reference" > "$proof/reference.output"

"$cxx" "${common[@]}" -fPIE -c "$driver" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --project-root "$project" --project-entry Witness.pipe \
        --entry DiFixture.NativeConstruction.Witness.Run --host-symbol test_construction \
        --llc "$llc" --profile "$profile" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"
done

"$debugger" --batch --nx --quiet \
    -ex 'set debuginfod enabled off' -ex 'set disable-randomization off' \
    -ex "directory $project/src" -ex 'tbreak Root/RootDependency.pipe:9' -ex run \
    -ex 'set $expectedReceiver = identities' -ex step -ex 'bt 3' -ex next \
    -ex 'print this == $expectedReceiver' -ex continue "$proof/debug" > "$proof/debugger.output" 2>&1
grep -E '#0 .*IdentitySequence.Next.*IdentitySequence.pipe' "$proof/debugger.output"
grep -Fx '$1 = 1' "$proof/debugger.output"
grep -F "exited normally" "$proof/debugger.output"
cat "$proof/reference.output"
