#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
isolated=$(dirname "$app")
cd "$isolated"
test "$(find . -maxdepth 1 -type f | wc -l)" -eq 1

/usr/bin/readelf -lW "$app" > ../runtime-app.program-headers
/usr/bin/readelf -dW "$app" > ../runtime-app.dynamic
if grep -w INTERP ../runtime-app.program-headers || grep -F '(NEEDED)' ../runtime-app.dynamic; then
    printf '%s\n' 'The bundled application has a dynamic runtime dependency' >&2
    exit 1
fi

test "$(env -i "$app" 0)" = 7
test "$(env -i "$app" 1)" = 42
test "$(env -i "$app" 2)" = 2

status=0
env -i "$app" 3 > ../runtime-app-failure.stdout 2> ../runtime-app-failure.stderr || status=$?
test "$status" -eq 1
test ! -s ../runtime-app-failure.stdout
grep -Fx 'list index outside admitted range' ../runtime-app-failure.stderr

status=0
env -i "$app" 0 --cancel > ../runtime-app-cancel.stdout 2> ../runtime-app-cancel.stderr || status=$?
test "$status" -eq 2
test ! -s ../runtime-app-cancel.stdout
grep -Fx 'native invocation cancelled' ../runtime-app-cancel.stderr
printf '%s\n' 'Standalone collection results, bounds failure and cancellation passed.'
