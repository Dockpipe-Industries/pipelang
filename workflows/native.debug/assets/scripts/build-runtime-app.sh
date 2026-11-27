#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
source=$3
entry=$4
output=$5
profile=${6:-debug}
project_entry=${7:-}
source_arguments=(--source "$source")
if [[ -n "$project_entry" ]]; then
    source_arguments=(--project-root "$source" --project-entry "$project_entry" --source-libraries "$8")
fi
"$sdk/pipelang-objectc" "${source_arguments[@]}" --entry "$entry" \
    --target linux-x64 --executable --profile "$profile" --llc "$llc" \
    --link-sdk "$sdk" --link-sdk-sha256 "$(<"$sdk/link-sdk.sha256")" --linker /usr/bin/ld \
    --output "$output"
