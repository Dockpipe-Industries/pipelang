#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
proof=$(dirname "$app")
test "$(env -i "$app" 0 --require-suspension)" = 42
test "$(env -i "$app" 7 --require-suspension)" = 42
test "$(env -i "$app" -9223372036854775808 --require-suspension)" = 0

expect_error() {
    local name=$1
    local input=$2
    local message=$3
    local status=0
    env -i "$app" "$input" --require-suspension > "$proof/$name.stdout" 2> "$proof/$name.stderr" || status=$?
    test "$status" -eq 1
    test ! -s "$proof/$name.stdout"
    printf '%s\n' "$message" > "$proof/$name.expected"
    cmp "$proof/$name.expected" "$proof/$name.stderr"
}

expect_error overflow 4 'integer addition overflow'
expect_error zero-divisor 5 'integer division by zero'
expect_error evaluation-budget 6 'source evaluation budget exceeded'
printf '%s\n' 'Snapshot iteration, loop transfers, checked arithmetic, and the evaluation limit passed.'
