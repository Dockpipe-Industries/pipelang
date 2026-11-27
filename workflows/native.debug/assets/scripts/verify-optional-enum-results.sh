#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42
test "$(env -i "$app" 3 "$mode")" = 42

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

expect_error missing '1' 'Optional.Value requires Some' 2 "$mode"
expect_error retained-message '1' 'étape incomplète' 4 "$mode"
expect_error task-cancelled '2' 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Native enums and optional values survived list storage, async cleanup and checked projection.'
