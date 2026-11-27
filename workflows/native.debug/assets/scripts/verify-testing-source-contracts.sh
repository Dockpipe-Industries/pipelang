#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
testing="$PWD/sdk/libraries/testing"
mkdir "$proof"

digest=$(sha256sum "$testing/library.json" | cut -d ' ' -f 1)
/usr/bin/jq -n --arg root "$testing" --arg digest "$digest" '{
    protocol: "pipelang.source-library-references.v1",
    libraries: [{root: $root, descriptor: "library.json", sha256: $digest}]
}' > "$proof/references.json"

"$cxx" -std=c++20 -O2 -fPIE -Wall -Wextra -Wpedantic -Werror -pthread \
    -I "$sdk_include" -I "$PWD/src/native/third_party" -c "$assets/TestingSourceContracts.cpp" -o "$proof/driver.o"

emit() {
    local profile=$1
    local project=$2
    local fixture=$3
    local entry=$4
    shift 4
    "$sdk/pipelang-objectc" --project-root "$PWD/tests/$project" --project-entry Entry.pipe \
        --source-libraries "$proof/references.json" --entry "$entry" --host-symbol "proof_$fixture" \
        --profile "$profile" --llc "$llc" --output "$proof/$fixture-$profile.o" "$@"
}

for profile in debug release; do
    emit "$profile" testing ordinary PipeLang.Testing.SelfTests.SelfTests.Run \
        --host-method DeliberateFailure --host-method DuplicateRegistrationRefused
    emit "$profile" testing probe PipeLang.Testing.SelfTests.ProbeSuite.Run
    emit "$profile" testing-async async PipeLang.Testing.AsyncSelfTests.AsyncSuite.Run
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/ordinary-$profile.o" "$proof/probe-$profile.o" "$proof/async-$profile.o" \
        "$sdk/libpipelang_native_app_runtime.a" -o "$proof/suites-$profile"
    env -i "$proof/suites-$profile" > "$proof/$profile.output"
done

cmp "$proof/debug.output" "$proof/release.output"
sed -n '1p' "$proof/release.output" > "$proof/ordinary.output"
sed -n '2p' "$proof/release.output" > "$proof/probe.output"
sed -n '3p' "$proof/release.output" > "$proof/async.output"
cat "$proof/ordinary.output"
/usr/bin/jq -er '"PipeLang authored cancellation probes: \(.results | length) passed"' "$proof/probe.output"
cat "$proof/async.output"
