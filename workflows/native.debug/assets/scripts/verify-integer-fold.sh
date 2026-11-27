#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
export LC_ALL=C

compiler=$1
model_compiler=$2
compiler_record=$3
llc=$4
sdk_include=$5
runtime=$6
proof=$7
baseline=${8:-}
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
llvm_bin=$(dirname "$llc")
mkdir "$proof" "$proof/reference"

sha256sum "$compiler" "$model_compiler" "$cxx" "$llc" "$runtime" \
    "$assets/IntegerFold.pipe" "$assets/IntegerFoldBenchmark.cpp" > "$proof/inputs.sha256"
jq -n --rawfile source "$assets/IntegerFold.pipe" '{
    protocol: "pipelang.native-model-project.v1",
    entry: "NativeProof.IntegerFold",
    namespace: "reference",
    modules: [{name: "NativeProof.IntegerFold", path: "IntegerFold.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference/response.json"
jq -er 'select(.ok == true) | .header' "$proof/reference/response.json" > "$proof/reference/Model.hpp"
jq -er 'select(.ok == true) | .types_header' "$proof/reference/response.json" > "$proof/reference/Types.hpp"
"$cxx" -std=c++20 -O2 -pthread -DCPP_REFERENCE -I "$sdk_include" -I "$proof/reference" \
    "$assets/IntegerFoldBenchmark.cpp" -o "$proof/cpp-reference"
env -i "$proof/cpp-reference"

build() {
    local driver=$1
    local profile=$2
    local name=$3
    "$driver" --source "$assets/IntegerFold.pipe" --entry NativeProof.IntegerFold.Session.Run \
        --host-symbol proof_integer_fold --llc "$llc" --profile "$profile" --output "$proof/$name.o"
    "$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" "$assets/IntegerFoldBenchmark.cpp" \
        "$proof/$name.o" "$runtime" -o "$proof/$name"
    env -i "$proof/$name"
}

build "$compiler" debug candidate-debug
build "$compiler" release candidate
"$llvm_bin/llvm-readobj" --relocations "$proof/candidate.o" > "$proof/candidate.relocations"
[[ $(rg -c 'pipelang_native_v19_integer' "$proof/candidate.relocations") == 3 ]]
if [[ -z "$baseline" ]]; then
    printf '%s\n' 'Native checked constant arithmetic and failure handling matched C++.'
    exit 0
fi

sha256sum "$baseline" >> "$proof/inputs.sha256"
uname -srm > "$proof/host"
awk -F ': ' '/^model name/ { print $2; exit }' /proc/cpuinfo >> "$proof/host"
build "$baseline" release baseline
"$llvm_bin/llvm-readobj" --relocations "$proof/baseline.o" > "$proof/baseline.relocations"
[[ $(rg -c 'pipelang_native_v19_integer' "$proof/baseline.relocations") == 12 ]]
printf 'compiler\tsample\twall_us\n' > "$proof/results.tsv"
for sample in {1..7}; do
    if ((sample % 2)); then
        modes=(baseline candidate cpp-reference)
    else
        modes=(cpp-reference candidate baseline)
    fi
    for mode in "${modes[@]}"; do
        elapsed=$(env -i "$proof/$mode" --measure)
        printf '%s\t%s\t%s\n' "$mode" "$sample" "$elapsed" >> "$proof/results.tsv"
    done
done
cat "$proof/results.tsv"
