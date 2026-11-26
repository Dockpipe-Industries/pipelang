#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
include=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
module=NativeProof.EvaluationBudget

mkdir "$proof"
jq -n --rawfile source "$assets/EvaluationBudget.pipe" --arg module_name "$module" '{
    protocol: "pipelang.native-model-project.v1", entry: $module_name, namespace: "reference",
    modules: [{name: $module_name, path: "EvaluationBudget.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
jq -er 'select(.ok == true) | .header' "$proof/reference.json" > "$proof/Model.hpp"
jq -er 'select(.ok == true) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror \
    -I "$include" -I "$proof" -c "$assets/EvaluationBudget.cpp" -o "$proof/driver.o"

for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$assets/EvaluationBudget.pipe" --entry "$module.Session.Run" \
        --host-symbol proof_evaluation_budget --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
