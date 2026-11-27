#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
export LC_ALL=C

native=$(realpath -e "$1")
reference=$(realpath -e "$2")
inputs=$(realpath -e "$3")
expected=$(realpath -e "$4")
proof=$(realpath -m "$5")
mkdir "$proof"

sha256sum "$native" "$reference" "$expected" > "$proof/identities.sha256"
(
    cd "$inputs"
    find . -type f -print0 | sort -z | xargs -0 sha256sum
) > "$proof/inputs.sha256"
uname -srm > "$proof/host"
awk -F ': ' '/^model name/ { print $2; exit }' /proc/cpuinfo >> "$proof/host"
printf 'backend\tsample\tseconds\tpeak_rss_kib\tuser_seconds\tsystem_seconds\texecutable_bytes\n' > "$proof/samples.tsv"

TIMEFORMAT='%R'

sample() {
    local backend=$1
    local app=$2
    local index=$3
    local elapsed rss user system
    elapsed=$( { time /usr/bin/time -f '%M %U %S' -o "$proof/usage" /usr/bin/env -i \
        "$app" "$inputs" "$proof/report.json" > "$proof/$backend.stdout"; } 2>&1 )
    cmp "$expected" "$proof/report.json"
    read -r rss user system < "$proof/usage"
    if [[ "$index" != warmup ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$backend" "$index" "$elapsed" "$rss" "$user" "$system" \
            "$(stat -c %s "$app")" >> "$proof/samples.tsv"
    fi
}

for index in {1..3}; do
    sample native "$native" warmup
    sample reference "$reference" warmup
done
for index in {1..21}; do
    if ((index % 2)); then
        sample native "$native" "$index"
        sample reference "$reference" "$index"
    else
        sample reference "$reference" "$index"
        sample native "$native" "$index"
    fi
done
