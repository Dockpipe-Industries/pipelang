#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
test "$(env -i "$app" 0 --require-suspension)" = 7
test "$(env -i "$app" 38 --require-suspension)" = 7
test "$(env -i "$app" 39 --require-suspension)" = 42
test "$(env -i "$app" 42 --require-suspension)" = 42

status=0
env -i "$app" 42 --cancel-after-suspension || status=$?
test "$status" -eq 2
printf '%s\n' 'Constructors, receiver text and aliases, independent state, async resumption, and cancellation passed.'
