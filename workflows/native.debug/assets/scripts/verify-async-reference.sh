#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

model_compiler=$1
compiler_record=$2
proof=$3
source_path=$4
sdk_include=$5
entry=$6
verify=$7
shift 7
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source_path" --arg entry "$entry" --arg path "$(basename "$source_path")" '{
    protocol: "pipelang.native-model-project.v1",
    entry: $entry,
    namespace: "reference",
    modules: [{name: $entry, path: $path, source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -e -r 'select(.ok == true) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -e -r 'select(.ok == true) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" -I "$proof" \
    "$assets/AsyncReference.cpp" -o "$proof/async-reference"
/bin/bash "$verify" "$proof/async-reference" "$@"
