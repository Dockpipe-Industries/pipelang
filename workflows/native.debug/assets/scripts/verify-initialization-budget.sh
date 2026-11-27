#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

native=$1
reference=$2
proof=$3

mkdir "$proof"
env -i "$reference" --budget-boundaries > "$proof/reference.txt"
for profile in debug release; do
    env -i "$native/$profile" --budget-boundaries > "$proof/$profile.txt"
    diff -u "$proof/reference.txt" "$proof/$profile.txt"
done
printf '%s\n' 'Native and reference initialization budgets preserved the same outcomes and host effects.'
