#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 1 "$mode")" = 42

expect_error() {
    local name=$1
    local expected_status=$2
    local message=$3
    shift 3
    local status=0
    env -i "$app" "$@" > "$proof/$name.stdout" 2> "$proof/$name.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/$name.stdout"
    printf '%s\n' "$message" > "$proof/$name.expected"
    cmp "$proof/$name.expected" "$proof/$name.stderr"
}

expect_error owned-error 1 'échec "source"' 0 "$mode"
expect_error lexical-rethrow 2 'preserve the caught message' 2 "$mode"
expect_error evaluation-limit 1 'source evaluation budget exceeded' 3 "$mode"
expect_error cancellation 2 'operation cancelled' 4 --cancel-after-suspension
expect_error failure-result 1 'authored failure result' 5 --require-suspension
expect_error checked-value 1 'Result.Value requires Success' 6 "$mode"
printf '%s\n' 'Native catch recovered from source faults and preserved rethrow, cancellation and terminal behavior.'
