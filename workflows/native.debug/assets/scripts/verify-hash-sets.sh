#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
[[ $(env -i "$app" 1 --require-suspension) == 7 ]]
status=0
failure=$(env -i "$app" -1 2>&1) || status=$?
[[ $status == 1 && $failure == "registration refused" ]]
printf '%s\n' 'Native HashSet preserved membership, insertion order and copies across suspension.'
