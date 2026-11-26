#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42

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

expect_error record-failure 1 'report unavailable' 1 "$mode"
expect_error record-cancelled 2 'report cancelled' 2 "$mode"
expect_error checked-value 1 'Result.Value requires Success' 3 "$mode"
expect_error retained-text 1 'épreuve structurée' 4 "$mode"
expect_error task-cancelled 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Native structured results preserved nested fields, result tags and UTF-8 through awaits.'
