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
source="$PWD/tests/native-models/Fundamentals.pipe"

mkdir -p "$proof/project/src"
cat > "$proof/project/src/Entry.pipe" <<'PIPE'
module Floating.Host;
import Floating.Values;

public model Class Model {
    public Model() {}
    public pure float32 Decimal() => Literals.Decimal();
}
PIPE
cat > "$proof/project/src/Values.pipe" <<'PIPE'
module Floating.Values;

public Class Literals {
    public static pure float32 Decimal() => 0.1f;
}
PIPE

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$assets/HostedBinary32Environment.cpp" -o "$proof/driver.o"
for input in model project; do
    case "$input" in
        model) source_flags=(--model "$source" --entry Binary32.Decimal) ;;
        project) source_flags=(--project-root "$proof/project" --project-entry Entry.pipe --entry Floating.Host.Model.Decimal) ;;
    esac
    for profile in debug release; do
        for policy in portable active-host; do
            flags=()
            arguments=()
            if [[ "$policy" == active-host ]]; then
                flags=(--native-numeric-environment active-host)
                arguments=(--active-host)
            fi
            artifact="$proof/$input-$profile-$policy"
            "$sdk/pipelang-objectc" "${source_flags[@]}" --host-symbol proof_hosted_binary32 \
                --profile "$profile" --llc "$llc" "${flags[@]}" --output "$artifact.o"
            "$cxx" -pie -pthread "$proof/driver.o" "$artifact.o" "$sdk/libpipelang_native_app_runtime.a" -o "$artifact"
            env -i "$artifact" "${arguments[@]}"
        done
    done
done
