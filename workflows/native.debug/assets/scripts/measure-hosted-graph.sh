#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
objectc=$1
modelc=$2
cxx=$(<"$3")
llc=$4
include=$5
baseline=$6
candidate=$7
proof=$8
assets=$(cd "$(dirname "$0")/.." && pwd -P)
fixture="$assets/HostedInitialGraph.pipe"
driver="$assets/HostedInitialGraph.cpp"

mkdir "$proof"
jq -n --rawfile source "$fixture" '{
    protocol:"pipelang.native-model-project.v1",entry:"NativeProof.HostedInitialGraph",namespace:"reference",
    modules:[{name:"NativeProof.HostedInitialGraph",path:"HostedInitialGraph.pipe",source:$source}]
}' | "$modelc" > "$proof/reference.json"
jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/Model.hpp"
jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -DNDEBUG -pthread -DPIPELANG_GRAPH_CPP_REFERENCE -I "$include" -I "$proof" \
    "$driver" -o "$proof/cpp"
"$cxx" -std=c++20 -O2 -DNDEBUG -pthread -I "$include" -c "$driver" -o "$proof/driver.o"
"$objectc" --source "$fixture" --entry NativeProof.HostedInitialGraph.Session.Run \
    --host-symbol proof_hosted_initial_graph --profile release --llc "$llc" --output "$proof/app.o"
"$cxx" -pthread "$proof/driver.o" "$proof/app.o" "$baseline" -o "$proof/baseline"
"$cxx" -pthread "$proof/driver.o" "$proof/app.o" "$candidate" -o "$proof/candidate"
for mode in baseline candidate cpp; do
    "$proof/$mode"
done
cpu=$(awk '$1=="Cpus_allowed_list:" { split($2,cpus,/[,-]/); print cpus[1]; }' /proc/self/status)
sha256sum "$fixture" "$driver" "$modelc" "$objectc" "$cxx" "$llc" "$baseline" "$candidate" > "$proof/inputs.sha256"
printf 'sample\tbackend\tnanoseconds\n' > "$proof/samples.tsv"
for sample in 1 2 3 4 5 6 7; do
    backends=(baseline candidate cpp)
    if ((sample % 2 == 0)); then
        backends=(cpp candidate baseline)
    fi
    for mode in "${backends[@]}"; do
        elapsed=$(/usr/bin/taskset -c "$cpu" "$proof/$mode" --benchmark)
        printf '%s\t%s\t%s\n' "$sample" "$mode" "$elapsed" >> "$proof/samples.tsv"
    done
done
cat "$proof/samples.tsv"
