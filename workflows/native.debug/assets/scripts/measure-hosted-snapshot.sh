#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
reference=$2
cxx=$3
llc=$4
include=$5
before_runtime=$6
after_runtime=$7
proof=$8
assets=$(cd "$(dirname "$0")/.." && pwd -P)
source="$assets/SnapshotList.pipe"
driver="$assets/SnapshotList.cpp"
mkdir "$proof"

/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    namespace: "reference",
    entry: "NativeProof.SnapshotList",
    modules: [{name: "NativeProof.SnapshotList", path: "SnapshotList.pipe", source: $source}]
}' | "$reference" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
/usr/bin/jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
flags=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I "$include")
"$cxx" "${flags[@]}" -I "$proof" -DPIPELANG_SNAPSHOT_CPP_REFERENCE "$driver" -o "$proof/reference"
"$cxx" "${flags[@]}" -fPIE -c "$driver" -o "$proof/driver.o"

"$compiler" --source "$source" --entry NativeProof.SnapshotList.Session.Append \
    --host-symbol proof_snapshot_list --host-snapshot --profile release --llc "$llc" --output "$proof/release.o"
"$cxx" -pie -pthread "$proof/driver.o" "$proof/release.o" "$before_runtime" -o "$proof/before"
"$cxx" -pie -pthread "$proof/driver.o" "$proof/release.o" "$after_runtime" -o "$proof/after"

sha256sum "$proof/release.o" "$source" "$driver" "$compiler" "$reference" "$cxx" "$llc" \
    "$proof/before" "$proof/after" "$proof/reference" "$before_runtime" "$after_runtime" > "$proof/inputs.sha256"
uname -a > "$proof/system"
lscpu > "$proof/processors"
cpu=$(awk '/^Cpus_allowed_list:/ { split($2, ids, /[,-]/); print ids[1]; exit }' /proc/self/status)
test -n "$cpu"
printf '%s\n' "$cpu" > "$proof/cpu"
for mode in before after reference; do
    /usr/bin/taskset -c "$cpu" /usr/bin/env -i "$proof/$mode" > "$proof/warm-$mode"
done

printf 'sample\tbackend\tregion_ns\tuser_s\tsystem_s\tpeak_rss_kib\n' > "$proof/samples.tsv"
modes=(before after reference)
for sample in {1..7}; do
    for offset in 0 1 2; do
        mode=${modes[(sample + offset) % 3]}
        /usr/bin/time -o "$proof/$sample-$mode.time" -f '%U\t%S\t%M' \
            /usr/bin/taskset -c "$cpu" /usr/bin/env -i "$proof/$mode" > "$proof/$sample-$mode.output"
        printf '%s\t%s\t%s\t%s\n' "$sample" "$mode" "$(cat "$proof/$sample-$mode.output")" \
            "$(cat "$proof/$sample-$mode.time")" >> "$proof/samples.tsv"
    done
done
cat "$proof/samples.tsv"
