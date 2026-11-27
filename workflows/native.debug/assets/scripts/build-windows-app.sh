#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
toolchain=$2
llc=$3
source=$4
entry=$5
output=$6
pdb=$7
"$sdk/pipelang-objectc" --source "$source" --entry "$entry" \
    --target windows-x64 --executable --llc "$llc" --linker "$toolchain/bin/ld.lld" \
    --link-sdk "$sdk" --link-sdk-sha256 "$(<"$sdk/link-sdk.sha256")" \
    --pdb "$pdb" --output "$output"
