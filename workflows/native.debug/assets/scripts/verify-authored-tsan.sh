#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
compiler_record=$2
llc=$3
runtime=$4
proof=$5
tsan_archive=${6:-}
tsan_archive_sha256=${7:-}
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
llvm_bin=$(dirname "$llc")
mkdir "$proof"
runtime_compile_options=()
if [[ -f "$compiler_record-id" && $(<"$compiler_record-id") == GNU ]]; then
    runtime_compile_options+=("@${runtime%/native}/compiler-profiles/gnu-conservative-gc.rsp")
fi

printf '%s\n' "$llc" > "$proof/llvm-code-generator"
cat > "$proof/sanitizer-llc" <<'SH'
#!/bin/bash
set -euo pipefail
proof=$(dirname "$0")
llc=$(<"$proof/llvm-code-generator")
llvm_bin=$(dirname "$llc")
sed '/^define /s/ !dbg !/ sanitize_thread !dbg !/' |
    "$llvm_bin/opt" -passes='tsan-module,function(tsan)' -o - |
    "$llc" "$@"
SH
chmod 0700 "$proof/sanitizer-llc"

tsan=(-std=c++20 -g1 -pthread -fsanitize=thread -fno-pie)
tsan_link=(-no-pie -pthread -fsanitize=thread)
tool_inputs=()
if [[ -n "$tsan_archive" || -n "$tsan_archive_sha256" ]]; then
    if [[ "$tsan_archive" != /* || ! -f "$tsan_archive" || ! "$tsan_archive_sha256" =~ ^[0-9a-f]{64}$ ]]; then
        printf '%s\n' 'Select an absolute ThreadSanitizer archive with its SHA-256.' >&2
        exit 1
    fi
    printf '%s  %s\n' "$tsan_archive_sha256" "$tsan_archive" | sha256sum --check --status
    tsan_link=(-no-pie -pthread -Wl,--whole-archive "$tsan_archive" -Wl,--no-whole-archive -ldl -lrt)
    tool_inputs+=("$tsan_archive")
fi
tsan_env=(/usr/bin/setarch x86_64 -R /usr/bin/env -i TSAN_OPTIONS=halt_on_error=1)
"$cxx" "${tsan[@]}" -O1 -c "$assets/ThreadSanitizerControl.cpp" -o "$proof/control.o"
"$cxx" "$proof/control.o" "${tsan_link[@]}" -o "$proof/control"
status=0
"${tsan_env[@]}" "$proof/control" > "$proof/control.output" 2>&1 || status=$?
test "$status" -ne 0
rg -m 1 'WARNING: ThreadSanitizer: data race' "$proof/control.output"

objects=()
for source in native_app_runtime native_host native_host_value NativeAtomics NativeBytes NativeDictionaries \
    NativeFloating NativeParallel NativeReceiverStorage NativeSequences NativeConcurrentSequences \
    NativeConcurrentDictionaries; do
    object="$proof/$source.o"
    optimization=-O1
    compile_options=()
    if [[ "$source" == native_app_runtime ]]; then
        optimization=-O0
        compile_options=("${runtime_compile_options[@]}")
    fi
    "$cxx" "${tsan[@]}" "$optimization" "${compile_options[@]}" -I "$runtime/include" \
        -c "$runtime/src/$source.cpp" -o "$object"
    objects+=("$object")
done

verify_fixture() {
    local fixture=$1
    local symbol=$2
    local profile label
    "$cxx" "${tsan[@]}" -O1 -I "$runtime/include" -c "$assets/$fixture.cpp" -o "$proof/$fixture-driver.o"
    for profile in debug release; do
        label="$fixture-$profile"
        "$compiler" --source "$assets/$fixture.pipe" --entry "NativeProof.$fixture.Session.Run" \
            --host-symbol "$symbol" --llc "$proof/sanitizer-llc" --profile "$profile" --output "$proof/$label.o"
        "$llvm_bin/llvm-readobj" --relocations "$proof/$label.o" > "$proof/$label.relocations"
        rg -q '__tsan_(read|write)' "$proof/$label.relocations"
        "$cxx" "$proof/$fixture-driver.o" "$proof/$label.o" "${objects[@]}" "${tsan_link[@]}" -o "$proof/$label"
        "${tsan_env[@]}" "$proof/$label"
    done
}

sha256sum "$compiler" "$cxx" "$llc" "$llvm_bin/opt" "${tool_inputs[@]}" "${objects[@]}" > "$proof/inputs.sha256"
verify_fixture ConcurrentSequences proof_concurrent_sequences
verify_fixture ConcurrentDictionaries proof_concurrent_dictionaries
if [[ -n "$tsan_archive" ]]; then
    verify_fixture HostedGraphAsync proof_hosted_graph_async
else
    printf '%s\n' 'Hosted timed-lock ThreadSanitizer coverage requires a clock-aware runtime archive.'
fi
