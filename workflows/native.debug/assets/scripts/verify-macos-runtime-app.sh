#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
cd "$(dirname "$app")"
/usr/bin/otool -L "$app" > ../app-libraries.txt
if tail -n +2 ../app-libraries.txt | awk '$1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\/Library\// { found = 1 } END { exit !found }'; then
    printf '%s\n' 'Expected only macOS system libraries.' >&2
    exit 1
fi

test "$(/usr/bin/env -i "$app" 0 --require-suspension)" = 2
test "$(/usr/bin/env -i "$app" 1 --require-suspension)" = 42

status=0
/usr/bin/env -i "$app" 3 --cancel-after-suspension > ../cancel.stdout 2> ../cancel.stderr || status=$?
test "$status" -eq 2
test ! -s ../cancel.stdout
grep -F 'cancelled' ../cancel.stderr
printf '%s\n' 'Native macOS ARM64 standalone suspension, collections and cancellation passed.'
