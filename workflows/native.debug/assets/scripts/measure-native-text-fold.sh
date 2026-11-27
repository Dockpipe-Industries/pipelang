#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
export LC_ALL=C

baseline=$(realpath -e "$1")
candidate=$(realpath -e "$2")
llc=$(realpath -e "$3")
sdk_include=$(realpath -e "$4")
runtime=$(realpath -e "$5")
cxx=$(realpath -e "$6")
proof=$(realpath -m "$7")
assets=$(cd "$(dirname "$0")/.." && pwd -P)
mkdir "$proof"

sha256sum "$baseline" "$candidate" "$llc" "$runtime" "$cxx" \
    "$assets/TextFold.pipe" "$assets/TextFoldBenchmark.cpp" > "$proof/inputs.sha256"
uname -srm > "$proof/host"
awk -F ': ' '/^model name/ { print $2; exit }' /proc/cpuinfo >> "$proof/host"

build() {
    local mode=$1
    local compiler=$2
    "$compiler" --source "$assets/TextFold.pipe" --entry NativeProof.TextFold.Session.Run \
        --host-symbol proof_text_fold --llc "$llc" --profile release --output "$proof/$mode.o"
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I "$sdk_include" \
        "$assets/TextFoldBenchmark.cpp" "$proof/$mode.o" "$runtime" \
        -Wl,--wrap=_Znwm -o "$proof/$mode"
    env -i "$proof/$mode" > "$proof/$mode.warmup.tsv"
}

build baseline "$baseline"
build candidate "$candidate"
llvm_readobj="$(dirname "$llc")/llvm-readobj"
for mode in baseline candidate; do
    "$llvm_readobj" --relocations "$proof/$mode.o" > "$proof/$mode.relocations"
done
baseline_calls=$(rg -c 'pipelang_native_v19_text_operation' "$proof/baseline.relocations")
candidate_calls=$(rg -c 'pipelang_native_v19_text_operation' "$proof/candidate.relocations")
[[ $baseline_calls == 2 && $candidate_calls == 1 ]]

printf 'compiler\tsample\twall_us\towner_thread_allocations\n' > "$proof/results.tsv"
for sample in {1..7}; do
    if ((sample % 2)); then
        modes=(baseline candidate)
    else
        modes=(candidate baseline)
    fi
    for mode in "${modes[@]}"; do
        metrics=$(env -i "$proof/$mode")
        printf '%s\t%s\t%s\n' "$mode" "$sample" "$metrics" >> "$proof/results.tsv"
    done
done
cat "$proof/results.tsv"
