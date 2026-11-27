#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

app=$1
[[ $(env -i "$app" 1 --require-suspension) == 42 ]]
status=0
failure=$(env -i "$app" -1 2>&1) || status=$?
[[ $status == 1 && $failure == "UTF-8 byte offset splits a scalar" ]]
printf '%s\n' 'Native text preserved UTF-8 across frames and enforced scalar boundaries and byte limits.'
