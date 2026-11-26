#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
proof=$(dirname "$app")

test "$(env -i "$app" 0 --require-suspension)" = 42
test "$(env -i "$app" 6 --require-suspension)" = 41

expect_error() {
    local name=$1
    local expected_status=$2
    local expected_message=$3
    shift 3
    local status=0
    env -i "$app" "$@" > "$proof/$name.stdout" 2> "$proof/$name.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/$name.stdout"
    printf '%s\n' "$expected_message" > "$proof/$name.expected"
    cmp "$proof/$name.expected" "$proof/$name.stderr"
}

expect_error failure 1 'échec "workflow"' 1 --require-suspension
expect_error cancellation 2 'source cancelled' 2 --require-suspension
expect_error empty-cancellation 2 '' 3 --require-suspension
expect_error invalid-value 1 'Result.Value requires Success' 4 --require-suspension
expect_error success-error 1 '' 5 --require-suspension
expect_error pending-cancellation 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Result status, UTF-8 payload propagation, and checked Value access passed.'
