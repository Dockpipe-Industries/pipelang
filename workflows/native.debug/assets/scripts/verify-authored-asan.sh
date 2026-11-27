#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
compiler_record=$2
llc=$3
sdk_include=$4
runtime=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
llvm_bin=$(dirname "$llc")
mkdir "$proof"

printf '%s\n' "$llc" > "$proof/llvm-code-generator"
cat > "$proof/sanitizer-llc" <<'SH'
#!/bin/bash
set -euo pipefail
proof=$(dirname "$0")
llc=$(<"$proof/llvm-code-generator")
llvm_bin=$(dirname "$llc")
sed '/^define /s/ !dbg !/ sanitize_address !dbg !/' |
    "$llvm_bin/opt" -passes='asan-module' -o - |
    "$llc" "$@"
SH
chmod 0700 "$proof/sanitizer-llc"

asan=("-fsanitize=address" "-no-pie")
asan_env=(/usr/bin/env -i ASAN_OPTIONS=detect_leaks=1:halt_on_error=1)
"$llvm_bin/opt" -passes='asan-module' "$assets/AddressSanitizerControl.ll" -o "$proof/control.bc"
"$llc" -O0 -filetype=obj "$proof/control.bc" -o "$proof/control.o"
"$cxx" "${asan[@]}" "$proof/control.o" -o "$proof/control"
status=0
"${asan_env[@]}" "$proof/control" > "$proof/control.output" 2>&1 || status=$?
test "$status" -ne 0
rg -m 1 'ERROR: AddressSanitizer: heap-buffer-overflow' "$proof/control.output"

verify_fixture() {
    local fixture=$1
    local symbol=$2
    shift 2
    local profile label
    "$cxx" -std=c++20 -O2 -pthread -I "$sdk_include" \
        -c "$assets/$fixture.cpp" -o "$proof/$fixture-driver.o"
    for profile in debug release; do
        label="$fixture-$profile"
        "$compiler" --source "$assets/$fixture.pipe" --entry "NativeProof.$fixture.Session.Run" \
            --host-symbol "$symbol" --llc "$proof/sanitizer-llc" --profile "$profile" --output "$proof/$label.o" "$@"
        "$llvm_bin/llvm-readobj" --relocations "$proof/$label.o" > "$proof/$label.relocations"
        rg -q '__asan_report_(load|store)' "$proof/$label.relocations"
        "$cxx" -pthread "${asan[@]}" "$proof/$fixture-driver.o" "$proof/$label.o" "$runtime" -o "$proof/$label"
        "${asan_env[@]}" "$proof/$label"
    done
}

sha256sum "$compiler" "$cxx" "$llc" "$llvm_bin/opt" "$runtime" > "$proof/inputs.sha256"
verify_fixture FixedArrayClaims proof_fixed_array_claims --host-snapshot
verify_fixture HostedResultInterface proof_hosted_result_interface
verify_fixture ResultInterface proof_result_interface
verify_fixture SelfEscape proof_self_escape
verify_fixture HostedSynchronization proof_hosted_synchronization
verify_fixture HostedGraphCycles proof_hosted_graph_cycles
verify_fixture HostedGraphSynchronization proof_hosted_graph_synchronization
verify_fixture HostedInitialGraph proof_hosted_initial_graph
verify_fixture HostedInterfaceField proof_hosted_interface_field
verify_fixture HostedGraphAsync proof_hosted_graph_async
verify_fixture HostedModelFacetGraph proof_hosted_model_facet_graph
