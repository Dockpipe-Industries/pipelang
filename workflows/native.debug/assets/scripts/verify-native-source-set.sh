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

mkdir -p "$proof/src"
cp tests/native-models/Concurrent.pipe "$proof/src/"
printf '%s\n' 'module Values; public Record Box<T> { public T Value; }' > "$proof/src/Values.pipe"
cat > "$proof/source-set.json" <<'JSON'
{
    "protocol": "pipelang.native-model-source-set.v1",
    "entry": "App",
    "modules": [
        { "name": "App", "path": "src/Concurrent.pipe" },
        { "name": "Values", "path": "src/Values.pipe" }
    ]
}
JSON

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/OrdinaryConcurrent.cpp" -o "$proof/driver.o"
methods=(CompoundOperations SequenceOperations GenericModuleHelper ParallelShared QueueCount StackCount
         MapValue QueueSnapshot ParallelSnapshot OrdinarySnapshot AsyncLocalCapture FailAfterSharedEffects Cancelled
         ByteLimitPersistent TextCount FailedTextEntryAbsent PublicAsyncHandle)
method_arguments=()
for method in "${methods[@]}"; do
    method_arguments+=(--host-method "$method")
done
for profile in debug release; do
    "$sdk/pipelang-objectc" --source-set "$proof/source-set.json" --entry App.ConcurrentModel.DefaultCounts \
        --host-symbol proof_concurrent "${method_arguments[@]}" --profile "$profile" --llc "$llc" \
        --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done

# An explicit source set can use any descriptor extension. Preserve its bytes
# when the requested output names the same file through another directory path.
cp "$proof/source-set.json" "$proof/reserved.o"
ln -s "$proof" "$proof/alias"
if "$sdk/pipelang-objectc" --source-set "$proof/reserved.o" --entry App.ConcurrentModel.DefaultCounts \
    --host-symbol proof_concurrent --llc "$llc" --output "$proof/alias/reserved.o" > "$proof/refusal.output" 2>&1; then
    printf '%s\n' 'Native source-set build accepted an output aliasing the source descriptor' >&2
    exit 1
fi
grep -F 'native output cannot overwrite a source-set input' "$proof/refusal.output"
cmp "$proof/source-set.json" "$proof/reserved.o"
