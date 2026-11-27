#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

model_compiler=$1
compiler_record=$2
sdk_include=$3
proof=$4
fixture=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"
/usr/bin/jq -n --arg fixture "$fixture" --rawfile source "$assets/$fixture.pipe" '{
    protocol: "pipelang.native-model-project.v1",
    entry: ("NativeProof." + $fixture),
    namespace: "reference",
    modules: [{name: ("NativeProof." + $fixture), path: ($fixture + ".pipe"), source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -e -r 'select(.ok == true) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -e -r 'select(.ok == true) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" -I "$proof" \
    "$assets/${fixture}Reference.cpp" -o "$proof/reference"
env -i "$proof/reference"
