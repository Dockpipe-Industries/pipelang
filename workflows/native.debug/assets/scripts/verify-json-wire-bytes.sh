#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
json=$5
json_include=$6
json_archive=$7
proof=$8
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
mkdir -p "$proof/src"
cp "$assets/JsonWireBytes.pipe" "$proof/src/Entry.pipe"
hash=$(sha256sum "$json/library.json" | cut -d ' ' -f 1)
jq -n --arg root "$json" --arg digest "$hash" '{
    protocol: "pipelang.source-library-references.v1",
    libraries: [{root: $root, descriptor: "library.json", sha256: $digest}]
}' >"$proof/references.json"

for profile in debug release; do
    "$sdk/pipelang-objectc" --project-root "$proof" --project-entry Entry.pipe --source-libraries "$proof/references.json" \
        --entry NativeProof.JsonWireBytes.Session.Run --host-symbol proof_json_wire --profile "$profile" \
        --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" -I "$json_include" \
        "$assets/JsonWireBytes.cpp" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" "$json_archive" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
printf '%s\n' 'Native PipeLang preserved split UTF-16 wire bytes and closed accepted, rejected, and cancelled documents.'
