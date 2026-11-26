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

expect_error bounds 1 'list index outside admitted range' 2 "$mode"
expect_error retained-text 1 'épreuve d’origine' 4 "$mode"
expect_error aggregate-limit 1 'list aggregate element limit exceeded' 5 "$mode"
expect_error task-cancelled 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Owned native lists preserved aliases, forks, snapshots and nested values across suspension and return.'
