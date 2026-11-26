#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 39 "$mode")" = 42
test "$(env -i "$app" 4 "$mode")" = 42

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

expect_error failure 1 'child failed' 1 --require-suspension
expect_error cancellation 2 'child cancelled' 2 --require-suspension
expect_error execution-failure 1 'integer division by zero' 3 "$mode"
expect_error evaluation-limit 1 'source evaluation budget exceeded' 5
expect_error first-wait-cancelled 2 'operation cancelled' 39 --cancel-after-suspension
if [[ "$mode" = --require-two-suspensions ]]; then
    expect_error second-wait-cancelled 2 'operation cancelled' 39 --cancel-after-second-suspension
fi
printf '%s\n' 'Nested async calls preserved arguments, receiver mutation, failures and cancellation.'
