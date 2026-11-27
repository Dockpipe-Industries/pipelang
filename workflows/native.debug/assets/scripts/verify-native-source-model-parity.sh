#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
include=$4
fixtures=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")

mkdir "$proof"
interface_sources="$fixtures/empty-source-interfaces"
cp "$interface_sources/"{Consumer,AsyncTesting,TestingBase}.pipe "$proof/"
jq -n '{
    protocol: "pipelang.native-model-source-set.v1",
    entry: "Consumer",
    models: ["Consumer.App"],
    modules: [
        {name: "Consumer", path: "Consumer.pipe"},
        {name: "AsyncTesting", path: "AsyncTesting.pipe"},
        {name: "TestingBase", path: "TestingBase.pipe"}
    ]
}' > "$proof/interfaces.json"

member_arguments=(--model "$fixtures/MemberBindings.pipe" --model-export App --entry App.ItemInitial
                  --host-symbol proof_member_bindings)
for name in ItemInput ItemParameter InheritedInitial InheritedParameter Parameter ConstantParameter \
    NamedTypeParameter ReadHost Set First ExplicitCall GenericCall Workers Forward; do
    member_arguments+=(--host-method "$name")
done

interface_arguments=(--source-set "$proof/interfaces.json" --entry Consumer.App.EmptySuite
                     --host-symbol proof_empty_interfaces)
for name in EmptyNullable InvalidCase InvalidPure InvalidChild InvalidLater Required Run; do
    interface_arguments+=(--host-method "$name")
done

byte_iteration_arguments=(--model "$fixtures/ByteBufferIteration.pipe" --entry ByteBufferIteration.Sum
                          --host-symbol proof_byte_iteration)
for name in Typed Complement Control Snapshot ReceiverOnce Nested; do
    byte_iteration_arguments+=(--host-method "$name")
done

propagation_arguments=(--model "$fixtures/ResultPropagation.pipe" --entry ResultPropagation.Arguments
                       --host-symbol proof_typed_propagation)
for name in Binary And Or Choice NullableChoice WithCleanup Collection Assign ForInitializer While For ForNoContinue \
    Stored Exhaustion Trace CleanupCount CaughtCount CrossPayload Nested Void; do
    propagation_arguments+=(--host-method "$name")
done

verify() {
    local fixture=$1
    shift
    local output="$proof/$fixture"

    mkdir "$output"
    "$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
        -c "$assets/$fixture.cpp" -o "$output/driver.o"
    for profile in debug release; do
        "$sdk/pipelang-objectc" "$@" --host-object-references --host-snapshot \
            --profile "$profile" --llc "$llc" --output "$output/$profile.o"
        "$cxx" -pie -pthread "$output/driver.o" "$output/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
            -o "$output/$profile"
        env -i "$output/$profile"
    done
}

verify HostedMemberBindings "${member_arguments[@]}"
verify HostedEmptySourceInterfaces "${interface_arguments[@]}"
verify HostedByteIteration "${byte_iteration_arguments[@]}"
verify HostedTypedPropagation "${propagation_arguments[@]}"
