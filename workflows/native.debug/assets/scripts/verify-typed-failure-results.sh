#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42

expect_error() {
    local expected_status=$1
    local message=$2
    shift 2
    local status=0
    env -i "$app" "$@" > "$proof/failure.stdout" 2> "$proof/failure.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/failure.stdout"
    printf '%s\n' "$message" > "$proof/failure.expected"
    cmp "$proof/failure.expected" "$proof/failure.stderr"
}

expect_error 1 'étape bloquée' 1 "$mode"
expect_error 2 'étape bloquée' 2 "$mode"
expect_error 1 'Result.Value requires Success' 3 "$mode"
expect_error 1 'child cleanup failed' 4
expect_error 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Native typed errors and void results survived collection storage and async child returns.'
