#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")
test "$(env -i "$app" 0 "$mode")" = 42
test "$(env -i "$app" 6 "$mode")" = 42

expect_error() {
    local status=0
    local expected_status=$1
    local message=$2
    shift 2
    env -i "$app" "$@" > "$proof/propagation.stdout" 2> "$proof/propagation.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/propagation.stdout"
    printf '%s\n' "$message" | cmp - "$proof/propagation.stderr"
}

expect_error 1 'source reading failed' 1 "$mode"
expect_error 2 'source reading cancelled' 2 "$mode"
expect_error 1 'cleanup ran after propagation' 3 "$mode"
expect_error 1 'void validation failed' 4 "$mode"
expect_error 1 'source reading failed' 5 "$mode"
expect_error 2 'operation cancelled' 0 --cancel-after-suspension
printf '%s\n' 'Native try propagated success, typed failure, cancellation and cleanup across resumption.'
