#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
source_directory=$2
log=$3
debugger=$(/usr/bin/xcrun --find lldb)

if ! "$debugger" --batch --no-lldbinit \
    -o "settings set target.source-map . \"$source_directory\"" \
    -o 'breakpoint set --file SuspendingCollections.pipe --line 12' \
    -o 'run 1 --require-suspension' \
    -o 'frame variable index acquired' \
    -o 'thread backtrace' \
    -o 'thread step-over' \
    -o 'breakpoint disable 1' \
    -o 'continue' "$app" > "$log" 2>&1; then
    /bin/cat "$log"
    exit 1
fi

/bin/cat "$log"
/usr/bin/grep -F 'stop reason = breakpoint' "$log"
/usr/bin/grep -F 'SuspendingCollections.pipe:12' "$log"
/usr/bin/grep -F 'SuspendingCollections.App.Read' "$log"
/usr/bin/grep -F 'index = 1' "$log"
/usr/bin/grep -F 'acquired = false' "$log"
/usr/bin/grep -E 'SuspendingCollections\.pipe:(13|15)' "$log"
/usr/bin/grep -F 'exited with status = 0' "$log"
