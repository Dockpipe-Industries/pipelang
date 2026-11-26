#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
shift
launcher=("$@")
cd "$(dirname "$app")"
test "$(find . -maxdepth 1 -type f | wc -l)" -eq 1

/usr/bin/readelf -lW "$app" > ../suspending-app.program-headers
/usr/bin/readelf -dW "$app" > ../suspending-app.dynamic
if grep -w INTERP ../suspending-app.program-headers || grep -F '(NEEDED)' ../suspending-app.dynamic; then
    printf '%s\n' 'The suspending application has a dynamic runtime dependency' >&2
    exit 1
fi

test "$(env -i "${launcher[@]}" "$app" 0 --require-suspension)" = 2
test "$(env -i "${launcher[@]}" "$app" 1 --require-suspension)" = 42

failed() {
    local expected_status=$1
    local expected_message=$2
    local name=$3
    shift 3
    local status=0

    env -i "${launcher[@]}" "$app" "$@" > "../$name.stdout" 2> "../$name.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "../$name.stdout"
    grep -F -- "$expected_message" "../$name.stderr"
}

failed 1 'index outside admitted range' array-bounds 2 --require-suspension
failed 1 'list index outside admitted range' boolean-list-bounds 3 --require-suspension
failed 2 'cancelled' suspended-cancellation 3 --cancel-after-suspension
failed 1 'native task did not suspend twice' missing-second-wait 1 --require-two-suspensions
printf '%s\n' 'Suspension, resumption, both collection bounds, and cancellation while pending passed.'
