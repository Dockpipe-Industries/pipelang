#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"

methods=(LocalReads TemporaryResult ProjectScalars ProjectNested ProjectImageValue ProjectListIsolation
         TemporaryProjection ProjectAcrossHost ScalarReads RecordReads NestedRecordReads NestedValues CellValue
         RetainSelectedCell ReplaceIndex AppendIndex RollbackSelected Reset Size First Effects)
method_arguments=()
for method in "${methods[@]}"; do
    method_arguments+=(--host-method "$method")
done

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/NativeListReads.cpp" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$PWD/tests/native-models/ListReadObjects.pipe" \
        --entry ListReadObjects.App.Read --host-symbol proof_list_reads "${method_arguments[@]}" \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
