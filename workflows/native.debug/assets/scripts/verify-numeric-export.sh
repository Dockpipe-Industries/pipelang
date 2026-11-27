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
compiler="$sdk/pipelang-objectc"
module=NativeProof.NumericExport

mkdir "$proof"
jq -n --rawfile source "$assets/NumericExport.pipe" --arg module_name "$module" '{
    protocol: "pipelang.native-model-project.v1", entry: $module_name, namespace: "reference",
    native_numeric_environment: "active-host",
    modules: [{name: $module_name, path: "NumericExport.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
jq -er 'select(.ok == true) | .header' "$proof/reference.json" > "$proof/Model.hpp"
jq -er 'select(.ok == true) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"
jq -er 'select(.ok == true) | .numeric_header' "$proof/reference.json" > "$proof/Numeric.hpp"

floating_flags=(-frounding-math -ftrapping-math -ffp-contract=off)
if [[ $(<"${compiler_record}-id") == GNU ]]; then
    floating_flags+=(-fsignaling-nans)
else
    floating_flags+=(-ffp-model=strict)
fi
"$cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -DPIPELANG_ACTIVE_HOST_FLOATING=1 \
    "${floating_flags[@]}" \
    -I "$include" -I "$proof" -c "$assets/NumericExport.cpp" -o "$proof/driver.o"

"$cxx" -x c -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -c "$assets/NumericRecordAbi.c" -o "$proof/record-caller.o"

methods=(Narrow Widen Add Divide LiteralDivide ThroughHelper SingleFromInteger WideFromInteger
    SignedIdentity UnsignedIdentity Choose AdjustPoint NestPoint PointTag)
symbols=(pipe_narrow pipe_widen pipe_add pipe_divide pipe_literal_divide pipe_qualified_add pipe_single pipe_wide
    pipe_signed_identity pipe_unsigned_identity pipe_choose pipe_adjust_point pipe_nest_point pipe_point_tag)
arguments=(--source "$assets/NumericExport.pipe" --target linux-x64 --llc "$llc" --native-numeric-environment active-host)
for profile in debug release; do
    objects=()
    for index in "${!methods[@]}"; do
        method=${methods[index]}
        object="$proof/$profile-$method.o"
        "$compiler" "${arguments[@]}" --entry "$module.Queries.$method" \
            --numeric-symbol "${symbols[index]}" --profile "$profile" --output "$object"
        undefined=$(nm --undefined-only "$object")
        test -z "$undefined"
        objects+=("$object")
    done
    "$cxx" "$proof/driver.o" "$proof/record-caller.o" "${objects[@]}" -o "$proof/$profile"
    env -i "$proof/$profile"
done

previous="$proof/debug-Narrow.o"
sha256sum "$previous" > "$proof/last-good.sha256"
expectRefusal() {
    local label=$1 message=$2
    shift 2
    if "$compiler" "${arguments[@]}" --output "$previous" "$@" > "$proof/$label.output" 2>&1; then
        printf '%s unexpectedly compiled\n' "$label" >&2
        return 1
    fi
    grep -F "$message" "$proof/$label.output"
    sha256sum --check --status "$proof/last-good.sha256"
}
expectRefusal unsafe 'outside the checked native numeric profile' \
    --entry "$module.Queries.Unsafe" --numeric-symbol refused
expectRefusal unimplemented 'unsupported floating operation in native numeric export' \
    --entry "$module.Queries.Unsupported" --numeric-symbol refused
expectRefusal empty 'native numeric export requires a primitive numeric or nonempty numeric record return type' \
    --entry "$module.Queries.EmptyRecord" --numeric-symbol refused
expectRefusal hosted 'requires a separate active-host numeric object' \
    --entry "$module.Queries.Narrow" --numeric-symbol refused --host-symbol hosted
expectRefusal duplicate 'duplicate argument --numeric-symbol' \
    --entry "$module.Queries.Narrow" --numeric-symbol refused --numeric-symbol duplicate
expectRefusal reserved 'must not use a native runtime symbol' \
    --entry "$module.Queries.Narrow" --numeric-symbol pipelang_native_reserved
