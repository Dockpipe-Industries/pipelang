#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")

for scenario in 0 3 4; do
    test "$(env -i "$app" "$scenario" "$mode")" = 42
done

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

expect_error primary-fault 1 'integer division by zero' 1 "$mode"
expect_error cleanup-fault 1 'Result.Value requires Success' 2 "$mode"
expect_error terminal-fault 1 'source evaluation budget exceeded' 5 "$mode"
expect_error body-cancelled 2 'operation cancelled' 0 --cancel-after-suspension
if [[ "$mode" = --require-two-suspensions ]]; then
    test "$(env -i "$app" 0 --cancel-after-second-suspension)" = 42
fi

printf '%s\n' 'Awaited finally preserved source transfers, failure precedence and cancellation.'
