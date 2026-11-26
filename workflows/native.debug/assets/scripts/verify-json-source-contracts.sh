#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
json_archive=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
project="$PWD/tests/json"
json="$PWD/sdk/libraries/json"
testing="$PWD/sdk/libraries/testing"

mkdir "$proof"
/usr/bin/jq -n --arg json "$json" --arg testing "$testing" \
    --arg json_digest "$(sha256sum "$json/library.json" | cut -d ' ' -f 1)" \
    --arg testing_digest "$(sha256sum "$testing/library.json" | cut -d ' ' -f 1)" '{
    protocol: "pipelang.source-library-references.v1",
    libraries: [
        {root: $json, descriptor: "library.json", sha256: $json_digest},
        {root: $testing, descriptor: "library.json", sha256: $testing_digest}
    ]
}' > "$proof/references.json"

compile=(-std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include"
    -I "$PWD/modules/json/include" -I "$project")
"$cxx" "${compile[@]}" -c "$assets/JsonParserSourceContracts.cpp" -o "$proof/parser-driver.o"
"$cxx" "${compile[@]}" -c "$assets/JsonAdditionalSourceContracts.cpp" -o "$proof/additional-driver.o"

emit() {
    local profile=$1
    local fixture=$2
    local entry=$3
    shift 3
    "$sdk/pipelang-objectc" --project-root "$project" --project-entry Entry.pipe \
        --source-libraries "$proof/references.json" --entry "$entry" --host-symbol "proof_$fixture" \
        --profile "$profile" --llc "$llc" --output "$proof/$fixture-$profile.o" "$@"
}

for profile in debug release; do
    emit "$profile" parser PipeLang.Json.Tests.JsonSuite.Run
    emit "$profile" string_index PipeLang.Json.Tests.StringIndexSuite.Open --host-method Insert --host-method AtCapacity \
        --host-method Cancelled --host-method CrossDocumentLookup --host-method Close
    emit "$profile" render PipeLang.Json.RenderTests.JsonRenderSuite.Exercise
    emit "$profile" writer PipeLang.Json.WriterTests.JsonWriterSuite.Write --host-method Project
    emit "$profile" wire PipeLang.Json.WireTests.WireInputSuite.Run

    runtime="$sdk/libpipelang_native_app_runtime.a"
    "$cxx" -pie -pthread "$proof/parser-driver.o" "$proof/parser-$profile.o" "$runtime" "$json_archive" \
        -o "$proof/parser-$profile"
    "$cxx" -pie -pthread "$proof/additional-driver.o" "$proof/string_index-$profile.o" "$proof/render-$profile.o" \
        "$proof/writer-$profile.o" "$proof/wire-$profile.o" "$runtime" "$json_archive" -o "$proof/additional-$profile"

    env -i "$proof/parser-$profile" | tee "$proof/parser-$profile.output"
    env -i "$proof/additional-$profile" "$proof/writer-$profile.json" | tee "$proof/additional-$profile.output"
done
cmp "$proof/parser-debug.output" "$proof/parser-release.output"
cmp "$proof/additional-debug.output" "$proof/additional-release.output"
cmp "$proof/writer-debug.json" "$proof/writer-release.json"
