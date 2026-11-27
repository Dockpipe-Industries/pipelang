#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
mode=$2
proof=$(dirname "$app")

test "$(env -i "$app" 39 "$mode")" = 42
status=0
env -i "$app" 0 --cancel-after-suspension > "$proof/infinite.stdout" 2> "$proof/infinite.stderr" || status=$?
test "$status" -eq 2
test ! -s "$proof/infinite.stdout"
grep -Fx 'operation cancelled' "$proof/infinite.stderr"
if [[ "$mode" = --require-two-suspensions ]]; then
    status=0
    env -i "$app" 39 --cancel-after-second-suspension > "$proof/second.stdout" 2> "$proof/second.stderr" || status=$?
    test "$status" -eq 2
    test ! -s "$proof/second.stdout"
    grep -Fx 'operation cancelled' "$proof/second.stderr"
fi
printf '%s\n' 'Multiple awaits, retained locals, snapshot iteration, and cancellation passed.'
