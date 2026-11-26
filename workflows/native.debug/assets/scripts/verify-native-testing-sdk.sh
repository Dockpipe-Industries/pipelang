#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
testing=$3
proof=$4
assets=$(cd "$(dirname "$0")/.." && pwd -P)
mkdir -p "$proof/src"
cp "$assets/TestingSdkCalls.pipe" "$proof/src/Entry.pipe"
hash=$(sha256sum "$testing/library.json" | cut -d ' ' -f 1)
/usr/bin/jq -n --arg root "$testing" --arg digest "$hash" '{
    protocol: "pipelang.source-library-references.v1",
    libraries: [{root: $root, descriptor: "library.json", sha256: $digest}]
}' > "$proof/references.json"

app="$proof/testing-sdk"
/bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$proof" \
    NativeProof.TestingSdkCalls.App.Run "$app" debug Entry.pipe "$proof/references.json"
test "$(env -i "$app" 0 --require-suspension)" = 42

expect_failure() {
    local scenario=$1
    local message=$2
    local status=0
    env -i "$app" "$scenario" --require-suspension > "$proof/fault.stdout" 2> "$proof/fault.stderr" || status=$?
    test "$status" -eq 1
    test ! -s "$proof/fault.stdout"
    printf '%s\n' "$message" | cmp - "$proof/fault.stderr"
}

expect_failure 1 'testing assertion'
expect_failure 2 'testing equality'
printf '%s\n' 'The PipeLang.Testing source library returned its typed assertion failures through native calls.'
