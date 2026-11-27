#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
sdk_include=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
fixtures="$PWD/tests/native-models"
cxx=$(<"$compiler_record")
compile=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
mkdir "$proof"

verify() {
    local name=$1
    local source=$2
    local model=$3
    local entry=$4
    local symbol=$5
    local driver=$6
    local reference_driver=$7
    local reference_header=$8
    local reference_namespace=$9
    local protocol=${10}
    shift 10
    local build="$proof/$name"
    local fixture
    fixture=$(basename "$source" .pipe)
    local profile
    mkdir "$build"
    /usr/bin/jq -n --rawfile source "$source" --arg model "$model" --arg namespace "$reference_namespace" \
        --arg protocol "$protocol" '{
            protocol: $protocol,
            namespace: $namespace,
            source: $source
        } + (if $protocol == "pipelang.native-model.v3" then {models: [$model]} else {} end)' \
        | "$model_compiler" > "$build/reference.json"
    /usr/bin/jq -er 'select(.ok) | .header' "$build/reference.json" > "$build/$reference_header"
    "$cxx" "${compile[@]}" -I "$build" "$fixtures/$reference_driver" -o "$build/reference"
    env -i "$build/reference"
    "$cxx" "${compile[@]}" -fPIE -c "$assets/$driver.cpp" -o "$build/driver.o"
    for profile in debug release; do
        "$sdk/pipelang-objectc" --source "$source" --entry "$fixture.$model.$entry" --host-symbol "$symbol" \
            --profile "$profile" --llc "$llc" --output "$build/$profile.o" "$@"
        "$cxx" -pie -pthread "$build/driver.o" "$build/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
            -o "$build/$profile"
        env -i "$build/$profile"
    done
}

verify nested-receivers "$fixtures/InitializationObjects.pipe" App First proof_initialization_objects InitializationObjectsNative \
    initialization_budget_objects.cpp InitializationObjects.hpp initialization_objects pipelang.native-model.v3 \
    --host-method HasSecond
mkdir "$proof/explicit-root-input"
explicit_source="$proof/explicit-root-input/InitializationObjects.pipe"
sed 's/second = new Holder();/this.second = new Holder();/' "$fixtures/InitializationObjects.pipe" > "$explicit_source"
grep -Fq 'this.second = new Holder();' "$explicit_source"
verify nested-receivers-explicit "$explicit_source" App First proof_initialization_objects InitializationObjectsNative \
    initialization_budget_objects.cpp InitializationObjects.hpp initialization_objects pipelang.native-model.v3 \
    --host-method HasSecond

verify helper-calls "$fixtures/InitializationBudget.pipe" InitializationBudget QueryWork proof_initialization_helpers InitializationHelpersNative \
    initialization_budget.cpp InitializationBudget.hpp pipelang_initialization pipelang.native-model.v1 \
    --host-method Consume
verify host-calls "$fixtures/ExecutionSegments.pipe" App Query proof_hosted_execution_segments ExecutionSegmentsNative \
    execution_segments_source.cpp Model.hpp execution_segments pipelang.native-model.v3 \
    --host-method PrimaryFailure
