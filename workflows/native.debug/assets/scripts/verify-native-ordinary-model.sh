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
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/OrdinarySynchronization.cpp" -o "$proof/driver.o"
methods=(LockReturn LockLoop LockFault LockBudget LocalSemaphore ExplicitLock SimpleWait InvalidCreate AsyncAlias
         SynchronousWorkerWait Work EnteredCount ReleasePermit PermitCount CleanedCount FinishOne
         ObserveCancelledWait EditMarker CleanupObserved AcquiredFault Timed ImmediateBudget SuspendedBudget)
method_arguments=()
for method in "${methods[@]}"; do
    method_arguments+=(--host-method "$method")
done
for profile in debug release; do
    "$sdk/pipelang-objectc" --model "$PWD/tests/native-models/Synchronization.pipe" \
        --entry SynchronizationModel.ParallelLocked --host-symbol proof_synchronization --host-snapshot \
        --host-multiple-tasks "${method_arguments[@]}" --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done
