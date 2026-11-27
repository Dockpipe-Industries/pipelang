#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

reference=$1
compiler_record=$2
proof=$3
source_path=$4
sdk_include=$5
fixture=$6
cxx=$(<"$compiler_record")

mkdir "$proof/conformance"
/usr/bin/jq -n --rawfile source "$source_path" '{
    protocol: "pipelang.native-model-project.v1",
    entry: "Select",
    namespace: "reference",
    modules: [{name: "Select", path: "Select.pipe", source: $source}]
}' | "$reference" > "$proof/conformance/reference.json"
/usr/bin/jq -e -r 'select(.ok == true) | .header' "$proof/conformance/reference.json" > "$proof/conformance/Model.hpp"
/usr/bin/jq -e -r 'select(.ok == true) | .types_header' "$proof/conformance/reference.json" > "$proof/conformance/Types.hpp"

for profile in debug release; do
    object="$proof/select.o"
    if [[ "$profile" == release ]]; then
        object="$proof/release.o"
    fi
    "$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" -I "$proof/conformance" \
        "$fixture" "$object" -o "$proof/conformance/$profile"
    "$proof/conformance/$profile"
done
