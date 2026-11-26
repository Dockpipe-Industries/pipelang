#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
[[ $(env -i "$app" 1 --require-two-suspensions) == 119 ]]
[[ $(env -i "$app" 4 --require-two-suspensions) == 128 ]]
status=0
failure=$(env -i "$app" -1 2>&1) || status=$?
[[ $status == 1 && $failure == "interface request changed across suspension" ]]
for option in --cancel-after-suspension --cancel-after-second-suspension; do
    status=0
    cancelled=$(env -i "$app" 1 "$option" 2>&1) || status=$?
    [[ $status == 2 && $cancelled == "operation cancelled" ]]
done
printf '%s\n' 'Native interface collections dispatched both implementations and preserved distinct objects across awaits.'
