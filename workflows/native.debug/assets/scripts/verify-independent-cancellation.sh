#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42

expect_cancelled() {
    local name=$1
    local message=$2
    shift 2
    local status=0
    env -i "$app" "$@" > "$proof/$name.stdout" 2> "$proof/$name.stderr" || status=$?
    test "$status" -eq 2
    test ! -s "$proof/$name.stdout"
    printf '%s\n' "$message" > "$proof/$name.expected"
    cmp "$proof/$name.expected" "$proof/$name.stderr"
}

expect_cancelled body 'operation cancelled' 1 --cancel-body-after-suspension
expect_cancelled body-untimed 'operation cancelled' 4 --cancel-body-after-suspension
expect_cancelled cleanup 'explicit cleanup cancelled' 2 --cancel-cleanup-after-second-suspension
expect_cancelled implicit 'operation cancelled' 5 --cancel-after-suspension
test "$(env -i "$app" 3 --cancel-cleanup-after-second-suspension)" = 42
printf '%s\n' 'Explicit cancellation kept body, cleanup, unrelated and implicit tokens independent.'
