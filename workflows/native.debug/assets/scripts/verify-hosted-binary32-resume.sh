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
source="$proof/FloatSuspension.pipe"
cat > "$source" <<'PIPE'
public Class FloatSuspension {
    private SemaphoreSlim gate = SemaphoreSlim.Create(0, 1);

    public void Release() {
        gate.Release();
    }

    public async Task<Result<float32,string>> Run(float32 fraction) {
        await gate.WaitAsync();
        return Result<float32,string>.Success(1f + fraction);
    }
}
PIPE
"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/HostedBinary32Resume.cpp" -o "$proof/driver.o"
for profile in debug release; do
    for policy in portable active-host; do
        options=()
        arguments=()
        if [[ "$policy" == active-host ]]; then
            options=(--native-numeric-environment active-host)
            arguments=(--active-host)
        fi
        artifact="$proof/$profile-$policy"
        "$sdk/pipelang-objectc" --model "$source" --entry FloatSuspension.Run --host-symbol proof_float_suspension \
            --host-method Release --host-multiple-tasks "${options[@]}" --profile "$profile" --llc "$llc" \
            --output "$artifact.o"
        "$cxx" -pie -pthread "$proof/driver.o" "$artifact.o" "$sdk/libpipelang_native_app_runtime.a" -o "$artifact"
        env -i "$artifact" "${arguments[@]}"
    done
done
