#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42
test "$(env -i "$app" 3 "$mode")" = 42

expect_error() {
    local status=0
    local expected_status=$1
    local message=$2
    shift 2
    env -i "$app" "$@" > "$proof/fault.stdout" 2> "$proof/fault.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/fault.stdout"
    printf '%s\n' "$message" > "$proof/fault.expected"
    cmp "$proof/fault.expected" "$proof/fault.stderr"
}

expect_error 1 'policy rejected' 1
expect_error 1 'integer division by zero' 2
expect_error 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Native synchronous calls passed typed values, recovered faults, and ran cleanup.'
