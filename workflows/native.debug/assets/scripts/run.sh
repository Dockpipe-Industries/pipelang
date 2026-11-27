#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

: "${PIPELANG_NATIVE_DEBUG_PROOF:?Select a fresh absolute proof directory outside the checkout}"
: "${PIPELANG_NATIVE_DEBUG_BUILD:?Select a PipeLang SDK CMake build directory}"
: "${PIPELANG_NATIVE_DEBUG_RUNNER:?Select a qualified pipelang-contained executable}"
: "${PIPELANG_NATIVE_DEBUG_RUNNER_SHA256:?Pin the qualified runner SHA256}"
: "${PIPELANG_NATIVE_DEBUG_LLC:?Select an absolute LLVM llc executable}"
: "${PIPELANG_NATIVE_DEBUG_GDB:?Select an absolute GDB executable}"

proof=$PIPELANG_NATIVE_DEBUG_PROOF
build=$PIPELANG_NATIVE_DEBUG_BUILD
runner=$PIPELANG_NATIVE_DEBUG_RUNNER
linker_bin=${PIPELANG_NATIVE_DEBUG_LINKER_BIN:-}
qemu_arm64=${PIPELANG_NATIVE_DEBUG_QEMU_ARM64:-}
linux_arm64_sdk=${PIPELANG_NATIVE_DEBUG_LINUX_ARM64_SDK:-}
linux_arm64_lldb=${PIPELANG_NATIVE_DEBUG_LINUX_ARM64_LLDB:-}
windows_tools=${PIPELANG_NATIVE_DEBUG_WINDOWS_TOOLCHAIN:-}
tsan_archive=${PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE:-}
tsan_archive_sha256=${PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE_SHA256:-}
assets="$PWD/workflows/native.debug/assets"
source_path="$PWD/examples/native/Select.pipe"

for path in "$proof" "$build" "$runner" "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB"; do
    if [[ "$path" != /* || "$(realpath -m "$path")" != "$path" ]]; then
        printf 'Native debug requires a canonical absolute path: %s\n' "$path" >&2
        exit 1
    fi
done
if [[ "$proof/" == "$PWD/"* || "$build/" == "$PWD/"* ]]; then
    printf '%s\n' 'Native debug proof and build must be outside the source checkout' >&2
    exit 1
fi
if [[ -n "$windows_tools" ]]; then
    test "$windows_tools" = "$(realpath -e "$windows_tools")"
    test -x "$windows_tools/bin/x86_64-w64-mingw32-clang++"
    test -x "$windows_tools/bin/aarch64-w64-mingw32-clang++"
fi
if [[ -n "$linux_arm64_sdk" ]]; then
    test "$linux_arm64_sdk" = "$(realpath -e "$linux_arm64_sdk")"
    test -f "$linux_arm64_sdk/link-sdk.sha256"
    : "${linker_bin:?Select LLVM linkers for the Linux ARM64 SDK proof}"
fi
if [[ -n "$linux_arm64_lldb" ]]; then
    : "${linux_arm64_sdk:?Select the Linux ARM64 SDK for the debugger proof}"
    test "$linux_arm64_lldb" = "$(realpath -e "$linux_arm64_lldb")"
    test -x "$linux_arm64_lldb"
fi
for executable in "$runner" "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB"; do
    test -x "$executable"
done
printf '%s  %s\n' "$PIPELANG_NATIVE_DEBUG_RUNNER_SHA256" "$runner" | sha256sum --check --status
if [[ -n "$linker_bin" || -n "$qemu_arm64" ]]; then
    for path in "$linker_bin" "$qemu_arm64"; do
        if [[ "$path" != /* || "$(realpath -m "$path")" != "$path" ]]; then
            printf '%s\n' 'Cross-link verification requires both a canonical linker directory and a QEMU ARM64 executable' >&2
            exit 1
        fi
    done
    for executable in "$linker_bin/ld.lld" "$linker_bin/lld-link" "$linker_bin/ld64.lld" "$qemu_arm64"; do
        test -x "$executable"
    done
fi

if [[ "${1:-}" != --inside ]]; then
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 1800 -- \
        /usr/bin/env PIPELANG_NATIVE_DEBUG_PROOF="$proof" \
        PIPELANG_NATIVE_DEBUG_BUILD="$build" PIPELANG_NATIVE_DEBUG_RUNNER="$runner" \
        PIPELANG_NATIVE_DEBUG_RUNNER_SHA256="$PIPELANG_NATIVE_DEBUG_RUNNER_SHA256" \
        PIPELANG_NATIVE_DEBUG_LLC="$PIPELANG_NATIVE_DEBUG_LLC" \
        PIPELANG_NATIVE_DEBUG_GDB="$PIPELANG_NATIVE_DEBUG_GDB" \
        PIPELANG_NATIVE_DEBUG_LINKER_BIN="$linker_bin" PIPELANG_NATIVE_DEBUG_QEMU_ARM64="$qemu_arm64" \
        PIPELANG_NATIVE_DEBUG_LINUX_ARM64_SDK="$linux_arm64_sdk" \
        PIPELANG_NATIVE_DEBUG_LINUX_ARM64_LLDB="$linux_arm64_lldb" \
        PIPELANG_NATIVE_DEBUG_WINDOWS_TOOLCHAIN="$windows_tools" \
        PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE="$tsan_archive" \
        PIPELANG_NATIVE_DEBUG_TSAN_ARCHIVE_SHA256="$tsan_archive_sha256" \
        /bin/bash "$0" --inside
fi

: "${CONTAINED_JOB_SLICE:?Native debug verification requires aggregate containment}"
mkdir "$proof/units"

unit() {
    local name="$1"
    shift
    local prefix="$proof/units/$name"
    if ! "$runner" unit --output "$prefix" --estate "$proof" --timeout 900 -- "$@" > "$prefix.launcher.log" 2>&1; then
        tail -c 8000 "$prefix.output" >&2
        tail -c 3000 "$prefix.launcher.log" >&2
        return 1
    fi
    printf '%s: passed\n' "$name"
}

unit configure /usr/bin/cmake -S sdk/pipeview/model -B "$build" \
    -DCMAKE_BUILD_TYPE=Release -DPIPELANG_BUILD_NATIVE_OBJECT_PROOF=ON
unit build /usr/bin/cmake --build "$build" --parallel 1 --target pipelang-objectc pipelang-modelc \
    pipelang pipelang-sdk-tool pipelang_native_app_runtime pipelang_json_reader

compiler="$build/sdk-tools/pipelang-objectc"
llvm_bin=$(dirname "$PIPELANG_NATIVE_DEBUG_LLC")
sha256sum "$compiler" "$source_path" "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB" \
    "$llvm_bin/llvm-readobj" "$llvm_bin/llvm-dwarfdump" \
    "$assets/entry.s" /usr/bin/as /usr/bin/ld > "$proof/inputs.sha256"
if [[ -n "$linker_bin" ]]; then
    sha256sum "$linker_bin/ld.lld" "$linker_bin/lld-link" "$linker_bin/ld64.lld" "$qemu_arm64" \
        "$llvm_bin/llvm-dlltool" "$llvm_bin/llvm-mc" "$llvm_bin/llvm-pdbutil" \
        "$llvm_bin/llvm-ar" "$llvm_bin/dsymutil" \
        "$assets/run-checks.ll" "$assets/linux-arm64-entry.s" "$assets/windows-entry.ll" \
        "$assets/MachOApplicationLink.pipe" "$assets/macos-application-link.s" \
        "$assets/kernel32.def" "$assets/macos-entry.ll" >> "$proof/inputs.sha256"
fi
/usr/bin/cp "$source_path" "$proof/Select.pipe"
mkdir "$proof/repeat"
cp "$source_path" "$proof/repeat/Select.pipe"

arguments=(--entry Select.Selection.Select --llc "$PIPELANG_NATIVE_DEBUG_LLC")
unit object /usr/bin/strace -qq -f -e trace=execve,execveat -o "$proof/object.exec" \
    "$compiler" "${arguments[@]}" --source "$proof/Select.pipe" --output "$proof/select.o"
test "$(grep -Ec 'execve(at)?\(' "$proof/object.exec")" -eq 2
grep -F "execve(\"$compiler\"" "$proof/object.exec"
grep -F "execve(\"$PIPELANG_NATIVE_DEBUG_LLC\"" "$proof/object.exec"
unit repeat "$compiler" "${arguments[@]}" --source "$proof/repeat/Select.pipe" --output "$proof/repeat/select.o"
cmp "$proof/select.o" "$proof/repeat/select.o"
unit release "$compiler" "${arguments[@]}" --source "$proof/Select.pipe" --profile release --output "$proof/release.o"
unit startup /usr/bin/as --64 "$assets/entry.s" -o "$proof/entry.o"
unit link-debug /usr/bin/ld -o "$proof/select" "$proof/entry.o" "$proof/select.o"
unit link-release /usr/bin/ld -o "$proof/select-release" "$proof/entry.o" "$proof/release.o"
unit run-debug "$proof/select"
unit run-release "$proof/select-release"
unit release-lines /usr/bin/readelf --debug-dump=decodedline "$proof/select-release"
grep -F 'Select.pipe' "$proof/units/release-lines.output"
unit compare-select-backends /bin/bash "$assets/scripts/compare-select-backends.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof" "$source_path" "$PWD/sdk/pipeview/model/native/include" "$assets/SelectConformance.cpp"
unit native-project /bin/bash "$assets/scripts/verify-native-project.sh" \
    "$compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB" "$proof"
runtime_source="$PWD/examples/native/CollectionsAsync.pipe"
unit stage-runtime-sdk /bin/bash "$assets/scripts/stage-runtime-sdk.sh" \
    "$compiler" "$build/libpipelang_native_app_runtime.a" "$proof/runtime-sdk" \
    "$build/native-app-runtime-compiler" "$build/sdk-tools/pipelang-sdk-tool"
unit stage-native-cmake-package /bin/bash "$assets/scripts/stage-native-cmake-package.sh" "$build" "$proof/cmake-sdk"
unit native-cmake-executable /bin/bash "$assets/scripts/verify-cmake-native-executable.sh" \
    "$(<"$proof/cmake-sdk/package-directory")" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" /usr/bin/ld "$proof/cmake-consumer" "$build/sdk-tools/pipelang-sdk-tool"
unit native-cmake-object /bin/bash "$assets/scripts/verify-cmake-native-object.sh" \
    "$(<"$proof/cmake-sdk/package-directory")" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$proof/cmake-object-consumer"
unit native-single-file-object-model /bin/bash "$assets/scripts/verify-single-file-object-model.sh" \
    "$proof/runtime-sdk" "$build/sdk-tools/pipelang-sdk-tool" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/single-file-object-model"
unit native-cli-executable /bin/bash "$assets/scripts/verify-native-cli.sh" \
    "$build/legacy-compiler/pipelang" "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" /usr/bin/ld \
    "$(<"$proof/cmake-sdk/object-compiler-path")" "$proof/native-cli" "$(<"$proof/cmake-sdk/bin-directory")"
unit native-source-trace /bin/bash "$assets/scripts/verify-native-source-trace.sh" \
    "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$proof/native-source-trace"
process_arguments_source="$assets/ProcessArguments.pipe"
process_arguments_entry=NativeProof.ProcessArguments.App.Run
unit native-process-arguments /bin/bash "$assets/scripts/verify-process-arguments.sh" \
    "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$PIPELANG_NATIVE_DEBUG_GDB" "$proof/process-arguments"
unit uint32-values /bin/bash "$assets/scripts/verify-uint32.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/uint32-values"
unit checked-integer-fold /bin/bash "$assets/scripts/verify-integer-fold.sh" \
    "$compiler" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$build/libpipelang_native_app_runtime.a" "$proof/checked-integer-fold"
unit floating-values /bin/bash "$assets/scripts/verify-floating-values.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/floating-values"
unit active-host-narrowing /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/active-host-narrowing" ActiveNarrowing proof_active_narrowing \
    --native-numeric-environment active-host
unit active-host-numeric-export /bin/bash "$assets/scripts/verify-numeric-export.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/active-host-numeric-export"
unit hosted-binary32-environment /bin/bash "$assets/scripts/verify-hosted-binary32-environment.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/hosted-binary32-environment"
unit hosted-binary32-resume /bin/bash "$assets/scripts/verify-hosted-binary32-resume.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/hosted-binary32-resume"
unit evaluation-budget-boundaries /bin/bash "$assets/scripts/verify-evaluation-budget.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/evaluation-budget-boundaries"
unit byte-buffers /bin/bash "$assets/scripts/verify-byte-buffers.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/byte-buffers"
unit sequences /bin/bash "$assets/scripts/verify-sequences.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/sequences"
unit dictionaries /bin/bash "$assets/scripts/verify-dictionaries.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/dictionaries"
unit typed-host-results /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/typed-host-results" TypedHostResults proof_typed_host_results
unit hosted-synchronous /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-synchronous" HostedSynchronous proof_hosted_synchronous
unit result-interface /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/result-interface" ResultInterface proof_result_interface
unit reference-result-interface /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/result-interface-reference" ResultInterface
unit hosted-result-interface /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-result-interface" HostedResultInterface proof_hosted_result_interface
unit reference-hosted-result-interface /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-result-interface-reference" HostedResultInterface
unit hosted-inheritance /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-inheritance" HostedInheritance proof_hosted_inheritance
unit reference-hosted-inheritance /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-inheritance-reference" HostedInheritance
unit self-escape /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/self-escape" SelfEscape proof_self_escape
unit reference-self-escape /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/self-escape-reference" SelfEscape
unit async-recursion /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/async-recursion" AsyncRecursion proof_async_recursion
unit reference-async-recursion /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/async-recursion-reference" AsyncRecursion
unit contextual-recursion /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/contextual-recursion" ContextualRecursion proof_contextual_recursion
unit reference-contextual-recursion /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/contextual-recursion-reference" ContextualRecursion
unit inheritance /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/inheritance" Inheritance proof_inheritance
unit reference-inheritance /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/inheritance-reference" Inheritance
unit constructor-rollback /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/constructor-rollback" ConstructorRollback proof_constructor_rollback
unit reference-constructor-rollback /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/constructor-rollback-reference" ConstructorRollback
unit shared-semaphores /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/shared-semaphores" SharedSemaphores proof_shared_semaphores
unit reference-shared-semaphores /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/shared-semaphores-reference" SharedSemaphores
unit blocking-semaphores /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/blocking-semaphores" BlockingSemaphores proof_blocking_semaphores
unit reference-blocking-semaphores /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/blocking-semaphores-reference" BlockingSemaphores
cat "$proof/units/reference-blocking-semaphores.output" "$proof/units/reference-blocking-semaphores.output" \
    | cmp - "$proof/units/blocking-semaphores.output"
unit shared-atomics /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/shared-atomics" SharedAtomics proof_shared_atomics
unit reference-shared-atomics /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/shared-atomics-reference" SharedAtomics
unit lock-scopes /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/lock-scopes" LockScopes proof_lock_scopes
unit reference-lock-scopes /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/lock-scopes-reference" LockScopes
unit converted-foreach /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/converted-foreach" ConvertedForeach proof_converted_foreach
unit reference-converted-foreach /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/converted-foreach-reference" ConvertedForeach
unit parallel-workers /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/parallel-workers" ParallelWorkers proof_parallel_workers
unit reference-parallel-workers /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/parallel-workers-reference" ParallelWorkers
unit concurrent-sequences /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/concurrent-sequences" ConcurrentSequences proof_concurrent_sequences
unit reference-concurrent-sequences /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/concurrent-sequences-reference" ConcurrentSequences
unit concurrent-dictionaries /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/concurrent-dictionaries" ConcurrentDictionaries proof_concurrent_dictionaries
unit reference-concurrent-dictionaries /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/concurrent-dictionaries-reference" ConcurrentDictionaries
unit hosted-synchronization /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-synchronization" HostedSynchronization proof_hosted_synchronization
unit reference-hosted-synchronization /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-synchronization-reference" HostedSynchronization
unit hosted-object-composition /bin/bash "$assets/scripts/verify-hosted-object-composition.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-object-composition"
unit hosted-graph-cycles /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-cycles" HostedGraphCycles proof_hosted_graph_cycles
unit reference-hosted-graph-cycles /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-cycles-reference" HostedGraphCycles
unit hosted-graph-synchronization /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-synchronization" \
    HostedGraphSynchronization proof_hosted_graph_synchronization
unit reference-hosted-graph-synchronization /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-synchronization-reference" HostedGraphSynchronization
unit hosted-initial-graph /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-initial-graph" HostedInitialGraph proof_hosted_initial_graph
unit reference-hosted-initial-graph /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-initial-graph-reference" HostedInitialGraph
unit hosted-composition-construction /bin/bash "$assets/scripts/verify-hosted-composition-construction.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-composition-construction" \
    "$PIPELANG_NATIVE_DEBUG_GDB"
unit hosted-construction-cleanup /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-construction-cleanup" \
    HostedConstructionCleanup test_construction_cleanup
unit reference-hosted-construction-cleanup /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-construction-cleanup-reference" HostedConstructionCleanup
unit initialization-budget-boundaries /bin/bash "$assets/scripts/verify-initialization-budget.sh" \
    "$proof/hosted-construction-cleanup" "$proof/hosted-construction-cleanup-reference/reference" \
    "$proof/initialization-budget-boundaries"
unit hosted-assignment-order /bin/bash "$assets/scripts/verify-native-assignment-order.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/hosted-assignment-order"
unit hosted-method-table /bin/bash "$assets/scripts/verify-hosted-method-table.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/hosted-method-table"
unit hosted-interface-field /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-interface-field" HostedInterfaceField proof_hosted_interface_field
unit reference-hosted-interface-field /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-interface-field-reference" HostedInterfaceField
unit hosted-graph-async /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-async" HostedGraphAsync proof_hosted_graph_async
unit reference-hosted-graph-async /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-graph-async-reference" HostedGraphAsync
unit hosted-model-facet-graph /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-model-facet-graph" HostedModelFacetGraph proof_hosted_model_facet_graph
unit reference-hosted-model-facet-graph /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-model-facet-graph-reference" HostedModelFacetGraph
unit context-owners /bin/bash "$assets/scripts/verify-context-owners.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/context-owners"
unit hosted-composition-recipes /bin/bash "$assets/scripts/verify-hosted-composition-recipes.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-composition-recipes"
unit hosted-composition-graph /bin/bash "$assets/scripts/verify-hosted-composition-graph.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native" "$proof/hosted-composition-graph"
unit hosted-composition-operations /bin/bash "$assets/scripts/verify-hosted-composition-operations.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-composition-operations"
unit native-ordinary-source-set /bin/bash "$assets/scripts/verify-native-source-set.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/native-ordinary-source-set"
unit hosted-shared-inputs /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-shared-inputs" SharedHostInputs proof_borrowed \
    --host-method Signal --host-method ReadQueue --host-method FillQueue --host-method UpdateWithLock \
    --host-method UpdateAtomics --host-multiple-tasks
unit native-constructor-shared-fields /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/native-constructor-shared-fields" \
    ConstructorSharedFields proof_constructor_fields
unit reference-constructor-shared-fields /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/reference-constructor-shared-fields" ConstructorSharedFields
cat "$proof/units/reference-constructor-shared-fields.output" "$proof/units/reference-constructor-shared-fields.output" > "$proof/constructor-shared-fields.expected"
cmp "$proof/units/native-constructor-shared-fields.output" "$proof/constructor-shared-fields.expected"
unit native-ordinary-model /bin/bash "$assets/scripts/verify-native-ordinary-model.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/native-ordinary-model"
unit hosted-parallel-options /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-parallel-options" HostedParallelOptions proof_parallel_options
unit reference-parallel-options /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/reference-parallel-options" HostedParallelOptions
unit native-list-reads /bin/bash "$assets/scripts/verify-native-list-reads.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/native-list-reads"
for profile in debug release; do
    standalone_list="$proof/standalone-list-$profile"
    unit "build-standalone-list-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$assets/StandaloneList.pipe" NativeProof.StandaloneList.App.Run "$standalone_list" "$profile"
    unit "run-standalone-list-$profile" /usr/bin/env -i "$standalone_list"
    grep -Fxq 42 "$proof/units/run-standalone-list-$profile.output"
done
unit hosted-constructor-parallel /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-constructor-parallel" ConstructorParallel proof_constructor_parallel
unit reference-constructor-parallel /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/reference-constructor-parallel" ConstructorParallel
unit native-bounded-calls /bin/bash "$assets/scripts/verify-native-bounded-calls.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/native-bounded-calls"
unit hosted-collection-inputs /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-collection-inputs" HostedCollectionInputs proof_collection_inputs \
    --host-method Pop --host-method Park --host-method Workers --host-method View \
    --host-method Map --host-method Remove --host-method Ensure --host-method MapWorkers --host-method Clear
unit reference-collection-inputs /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/reference-collection-inputs" HostedCollectionInputs
unit hosted-constructor-inputs /bin/bash "$assets/scripts/verify-hosted-constructor-inputs.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-constructor-inputs"
unit reference-constructor-inputs /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/reference-constructor-inputs" ConstructorSharedInputs
nullable_conditions="$assets/NullableConditions.pipe"
for profile in debug release; do
    nullable_app="$proof/nullable-conditions-$profile"
    unit "build-nullable-conditions-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$nullable_conditions" NativeProof.NullableConditions.App.Run "$nullable_app" "$profile"
    unit "run-nullable-conditions-$profile" /usr/bin/env -i "$nullable_app"
    test "$(<"$proof/units/run-nullable-conditions-$profile.output")" = 42
done
unit scoped-owned-snapshots /bin/bash "$assets/scripts/verify-scoped-owned-snapshots.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/scoped-owned-snapshots"
unit scoped-collection-snapshots /bin/bash "$assets/scripts/verify-scoped-owned-snapshots.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/scoped-collection-snapshots" ScopedCollections
unit hosted-task-groups /bin/bash "$assets/scripts/verify-hosted-task-groups.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-task-groups"
unit hosted-initialization /bin/bash "$assets/scripts/verify-hosted-initialization.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-initialization"
unit hosted-snapshot /bin/bash "$assets/scripts/verify-hosted-snapshot.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-snapshot"
unit hosted-task-roots /bin/bash "$assets/scripts/verify-hosted-task-roots.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-task-roots"
unit hosted-constructor-selection /bin/bash "$assets/scripts/verify-hosted-constructor-selection.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-constructor-selection"
unit hosted-inherited-entry /bin/bash "$assets/scripts/verify-hosted-inherited-entry.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-inherited-entry"
unit hosted-array-initialization /bin/bash "$assets/scripts/verify-hosted-array-initialization.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-array-initialization"
unit hosted-object-references /bin/bash "$assets/scripts/verify-hosted-object-references.sh" \
    "$proof/runtime-sdk" "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-object-references"
unit floating-observation /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/floating-observation" FloatingObservation proof_floating_observation
unit reference-floating-observation /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/floating-observation-reference" FloatingObservation
unit hosted-call-values /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-call-values" HostedCallValues proof_hosted_call_values \
    --host-method Inspect --host-method Clear --host-method Check
unit hosted-argument-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'tbreak HostedCallValues.pipe:52' -ex run -ex 'set $caller_receiver = this' \
    -ex step -ex 'print scenario' -ex 'print this == $caller_receiver' \
    -ex 'tbreak HostedCallValues.pipe:42' -ex continue -ex 'print scenario' -ex continue "$proof/hosted-call-values/debug"
grep -E 'NativeProof.HostedCallValues.Session.CheckInput .* at HostedCallValues.pipe:41$' "$proof/units/hosted-argument-debugger.output"
grep -E 'NativeProof.HostedCallValues.Session.CheckInput .* at HostedCallValues.pipe:42$' "$proof/units/hosted-argument-debugger.output"
grep -Fx '$1 = 1' "$proof/units/hosted-argument-debugger.output"
grep -Fx '$2 = 1' "$proof/units/hosted-argument-debugger.output"
grep -Fx '$3 = 1' "$proof/units/hosted-argument-debugger.output"
grep -F 'exited normally' "$proof/units/hosted-argument-debugger.output"
unit persistent-sets /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/persistent-sets" PersistentSets proof_persistent_sets
unit reference-persistent-sets /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/persistent-sets-reference" PersistentSets
unit persistent-report-values /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/persistent-report-values" \
    PersistentReportValues proof_persistent_report_values
unit reference-persistent-report-values /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/persistent-report-values-reference" PersistentReportValues
unit fixed-array-claims /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/fixed-array-claims" FixedArrayClaims proof_fixed_array_claims --host-snapshot
unit reference-fixed-array-claims /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/fixed-array-claims-reference" FixedArrayClaims
unit array-values /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/array-values" ArrayValues proof_array_values
unit array-values-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break ArrayValues.pipe:27' -ex run -ex 'print before.labels.count' -ex bt -ex continue "$proof/array-values/debug"
grep -F 'NativeProof.ArrayValues.ArrayBuilder.Valid' "$proof/units/array-values-debugger.output"
grep -Fx '$1 = 3' "$proof/units/array-values-debugger.output"
grep -F 'exited normally' "$proof/units/array-values-debugger.output"
unit reference-array-values /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/array-values-reference" ArrayValues
unit array-host-values /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/array-host-values" ArrayHostValues proof_array_host_values
unit hosted-byte-streaming /bin/bash "$assets/scripts/verify-hosted-fixture.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-byte-streaming" HostedByteStreaming proof_hosted_byte_streaming
unit json-wire-bytes /bin/bash "$assets/scripts/verify-json-wire-bytes.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$PWD/sdk/libraries/json" "$PWD/modules/json/include" \
    "$build/json/libpipelang_json_reader.a" "$proof/json-wire-bytes"
unit json-source-contracts /bin/bash "$assets/scripts/verify-json-source-contracts.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$build/json/libpipelang_json_reader.a" "$proof/json-source-contracts"
unit native-testing-sdk /bin/bash "$assets/scripts/verify-native-testing-sdk.sh" \
    "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/libraries/testing" "$proof/native-testing-sdk"
unit testing-source-contracts /bin/bash "$assets/scripts/verify-testing-source-contracts.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/testing-source-contracts"
unit hosted-instance /bin/bash "$assets/scripts/verify-hosted-instance.sh" "$proof/runtime-sdk" \
    "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" "$PWD/sdk/pipeview/model/native/include" \
    "$proof/hosted-instance"
unit reference-hosted-clock /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-clock-reference" HostedClock
unit reference-hosted-receivers /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hosted-receivers-reference" HostedReceivers
mkdir "$proof/standalone" "$proof/interface-dispatch"
for profile in debug release; do
    interface_app="$proof/interface-dispatch/$profile"
    unit "build-interface-dispatch-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$assets/InterfaceDispatch.pipe" NativeProof.InterfaceDispatch.App.Run \
        "$interface_app" "$profile"
    unit "run-interface-dispatch-$profile" /bin/bash "$assets/scripts/verify-interface-dispatch.sh" "$interface_app"
    interface_array_app="$proof/interface-dispatch/array-$profile"
    unit "build-interface-array-dispatch-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$assets/InterfaceDispatch.pipe" NativeProof.InterfaceDispatch.App.RunArray \
        "$interface_array_app" "$profile"
    unit "run-interface-array-dispatch-$profile" /bin/bash "$assets/scripts/verify-interface-dispatch.sh" "$interface_array_app"
done
unit reference-interface-dispatch /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/interface-dispatch-reference" InterfaceDispatch
unit hosted-interface-inheritance /bin/bash "$assets/scripts/verify-hosted-interface-inheritance.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$PWD/tests/native-models" "$proof/hosted-interface-inheritance"
unit native-source-model-parity /bin/bash "$assets/scripts/verify-native-source-model-parity.sh" \
    "$proof/runtime-sdk" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$PWD/tests/native-models" "$proof/native-source-model-parity"
mkdir "$proof/hash-sets"
for profile in debug release; do
    hash_set_app="$proof/hash-sets/$profile"
    unit "build-hash-sets-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$assets/HashSets.pipe" NativeProof.HashSets.App.Run "$hash_set_app" "$profile"
    unit "run-hash-sets-$profile" /bin/bash "$assets/scripts/verify-hash-sets.sh" "$hash_set_app"
done
unit reference-hash-sets /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/hash-sets-reference" HashSets
unit native-call-graphs /bin/bash "$assets/scripts/verify-call-graphs.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$proof/call-graphs"
mkdir "$proof/text-operations"
for profile in debug release; do
    text_app="$proof/text-operations/$profile"
    unit "build-text-operations-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$assets/TextOperations.pipe" NativeProof.TextOperations.App.Run "$text_app" "$profile"
    unit "run-text-operations-$profile" /bin/bash "$assets/scripts/verify-text-operations.sh" "$text_app"
done
unit reference-text-operations /bin/bash "$assets/scripts/verify-hosted-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$PWD/sdk/pipeview/model/native/include" "$proof/text-operations-reference" TextOperations
unit build-text-pool-budget "$(<"$build/native-app-runtime-compiler")" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
    -I "$PWD/sdk/pipeview/model/native/include" -I "$PWD/sdk/pipeview/model/native/src" \
    "$assets/TextPoolBudget.cpp" -o "$proof/text-operations/pool-budget"
unit run-text-pool-budget /usr/bin/env -i "$proof/text-operations/pool-budget"
unit build-runtime-app /usr/bin/strace -qq -f -e trace=execve,execveat -o "$proof/runtime-app.exec" \
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$runtime_source" CollectionsAsync.App.Run "$proof/standalone/collections-async"
if grep -E 'execve\("[^"]*/(g\+\+|c\+\+|clang(\+\+)?|cc1plus)(-[0-9]+)?"' "$proof/runtime-app.exec"; then
    printf '%s\n' 'The application build invoked a C++ compiler' >&2
    exit 1
fi
unit run-runtime-app /bin/bash "$assets/scripts/verify-runtime-app.sh" "$proof/standalone/collections-async"
unit verify-executable-publication /bin/bash "$assets/scripts/verify-executable-publication.sh" \
    "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$runtime_source" "$proof/standalone"
unit runtime-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $(dirname "$runtime_source")" -ex 'set disable-randomization off' \
    -ex 'break CollectionsAsync.pipe:14' -ex 'run 2' -ex 'print count' \
    -ex bt -ex continue "$proof/standalone/collections-async"
grep -F 'CollectionsAsync.pipe:14' "$proof/units/runtime-debugger.output"
grep -F 'CollectionsAsync.App.Read' "$proof/units/runtime-debugger.output"
grep -Fx '$1 = 2' "$proof/units/runtime-debugger.output"
grep -F 'exited normally' "$proof/units/runtime-debugger.output"
standalone_source="$PWD/examples/native/Standalone.pipe"
for profile in debug release; do
    standalone="$proof/standalone/synchronous-$profile"
    unit "build-standalone-synchronous-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" \
        "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$standalone_source" Standalone.App.Run "$standalone" "$profile"
    unit "run-standalone-synchronous-$profile" /usr/bin/env -i "$standalone"
    grep -Fx 42 "$proof/units/run-standalone-synchronous-$profile.output"
    unit "reject-standalone-argument-$profile" /bin/bash -c '"$1" 9; test "$?" -eq 64' sh "$standalone"
    unit "cancel-standalone-synchronous-$profile" /bin/bash -c '"$1" --cancel; test "$?" -eq 2' sh "$standalone"
    unit "build-standalone-checked-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" \
        "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" "$standalone_source" Standalone.App.Checked "$standalone-checked" "$profile"
    unit "run-standalone-checked-$profile" /usr/bin/env -i "$standalone-checked" 41
    grep -Fx 42 "$proof/units/run-standalone-checked-$profile.output"
    unit "fail-standalone-checked-$profile" /bin/bash -c '"$1" -1; test "$?" -eq 1' sh "$standalone-checked"
    grep -Fx 'negative input' "$proof/units/fail-standalone-checked-$profile.output"
done
unit standalone-synchronous-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $(dirname "$standalone_source")" -ex 'set disable-randomization off' \
    -ex 'tbreak Standalone.pipe:10' -ex run -ex 'print answer' -ex bt -ex continue "$proof/standalone/synchronous-debug"
grep -F 'at Standalone.pipe:10' "$proof/units/standalone-synchronous-debugger.output"
grep -Fx '$1 = 42' "$proof/units/standalone-synchronous-debugger.output"
grep -F 'exited normally' "$proof/units/standalone-synchronous-debugger.output"
mkdir "$proof/suspending"
suspending_source="$PWD/examples/native/SuspendingCollections.pipe"
unit build-suspending-app /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$suspending_source" SuspendingCollections.App.Run "$proof/suspending/collections-async"
unit run-suspending-app /bin/bash "$assets/scripts/verify-suspending-app.sh" "$proof/suspending/collections-async"
unit suspending-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $(dirname "$suspending_source")" -ex 'set disable-randomization off' \
    -ex 'break SuspendingCollections.pipe:12' -ex 'run 1 --require-suspension' \
    -ex 'print acquired' -ex bt -ex continue "$proof/suspending/collections-async"
grep -F 'SuspendingCollections.pipe:12' "$proof/units/suspending-debugger.output"
grep -Fx '$1 = false' "$proof/units/suspending-debugger.output"
grep -F 'exited normally' "$proof/units/suspending-debugger.output"
if [[ -n "$linux_arm64_sdk" ]]; then
    unit linux-arm64-runtime-app /bin/bash "$assets/scripts/verify-linux-arm64-app.sh" \
        "$compiler" "$linux_arm64_sdk" "$proof/runtime-sdk" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$linker_bin/ld.lld" "$qemu_arm64" "$suspending_source" "$proof/linux-arm64-runtime"
fi
if [[ -n "$linux_arm64_lldb" ]]; then
    unit linux-arm64-debugger /bin/bash "$assets/scripts/verify-linux-arm64-debugger.sh" \
        "$qemu_arm64" "$linux_arm64_lldb" "$proof/linux-arm64-runtime/debug/standalone/suspending" \
        "$(dirname "$suspending_source")" "$proof/linux-arm64-runtime/debugger.txt"
fi
mkdir "$proof/owned-worker"
owned_source="$assets/OwnedWorker.pipe"
owned_app="$proof/owned-worker/owned-worker"
unit build-owned-worker /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$owned_source" NativeProof.OwnedWorker.App.Run "$owned_app"
unit run-owned-worker /bin/bash "$assets/scripts/verify-owned-worker.sh" "$owned_app"
unit owned-worker-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break OwnedWorker.pipe:19' -ex 'run 42 --require-suspension' \
    -ex bt -ex 'disable 1' -ex continue "$owned_app"
grep -F 'NativeProof.OwnedWorker.Worker.Read' "$proof/units/owned-worker-debugger.output"
grep -F 'NativeProof.OwnedWorker.Session.Execute' "$proof/units/owned-worker-debugger.output"
grep -F 'exited normally' "$proof/units/owned-worker-debugger.output"
mkdir "$proof/result-outcomes"
result_source="$assets/ResultOutcomes.pipe"
result_app="$proof/result-outcomes/result-outcomes"
unit build-result-outcomes /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$result_source" NativeProof.ResultOutcomes.App.Run "$result_app"
unit run-result-outcomes /bin/bash "$assets/scripts/verify-result-outcomes.sh" "$result_app"
mkdir "$proof/result-release"
result_release="$proof/result-release/result-outcomes"
unit build-result-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$result_source" NativeProof.ResultOutcomes.App.Run "$result_release" release
unit run-result-release /bin/bash "$assets/scripts/verify-result-outcomes.sh" "$result_release"
unit result-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break ResultOutcomes.pipe:37' -ex 'run 1 --require-suspension' \
    -ex 'print outcome.tag' -ex 'print outcome.error.size' "$result_app"
grep -F 'ResultOutcomes.pipe:37' "$proof/units/result-debugger.output"
grep -Fx '$1 = 2' "$proof/units/result-debugger.output"
grep -Fx '$2 = 17' "$proof/units/result-debugger.output"
unit reference-result-outcomes /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/result-reference" "$result_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.ResultOutcomes "$assets/scripts/verify-result-outcomes.sh"
mkdir "$proof/loop-outcomes"
loop_source="$assets/LoopOutcomes.pipe"
loop_app="$proof/loop-outcomes/loop-outcomes"
unit build-loop-outcomes /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$loop_source" NativeProof.LoopOutcomes.App.Run "$loop_app"
unit run-loop-outcomes /bin/bash "$assets/scripts/verify-loop-outcomes.sh" "$loop_app"
unit loop-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break LoopOutcomes.pipe:43' -ex 'run 7 --require-suspension' \
    -ex 'print item' -ex 'print index' -ex 'disable 1' -ex continue "$loop_app"
grep -F 'LoopOutcomes.pipe:43' "$proof/units/loop-debugger.output"
grep -Fx '$1 = 2' "$proof/units/loop-debugger.output"
grep -Fx '$2 = 0' "$proof/units/loop-debugger.output"
grep -F 'exited normally' "$proof/units/loop-debugger.output"
mkdir "$proof/loop-release"
loop_release="$proof/loop-release/loop-outcomes"
unit build-loop-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$loop_source" NativeProof.LoopOutcomes.App.Run "$loop_release" release
unit run-loop-release /bin/bash "$assets/scripts/verify-loop-outcomes.sh" "$loop_release"
unit reference-loop-outcomes /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/loop-reference" "$loop_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.LoopOutcomes "$assets/scripts/verify-loop-outcomes.sh"
mkdir "$proof/multiple-awaits"
await_source="$assets/MultipleAwaits.pipe"
await_app="$proof/multiple-awaits/multiple-awaits"
unit build-multiple-awaits /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$await_source" NativeProof.MultipleAwaits.App.Run "$await_app"
unit run-multiple-awaits /bin/bash "$assets/scripts/verify-multiple-awaits.sh" "$await_app" --require-two-suspensions
mkdir "$proof/await-release"
await_release="$proof/await-release/multiple-awaits"
unit build-await-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$await_source" NativeProof.MultipleAwaits.App.Run "$await_release" release
unit run-await-release /bin/bash "$assets/scripts/verify-multiple-awaits.sh" "$await_release" --require-two-suspensions
unit await-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break MultipleAwaits.pipe:31 if timeout == 45' -ex 'run 39 --require-two-suspensions' \
    -ex 'print completed' -ex 'print input' -ex 'print original.value' -ex 'continue' "$await_app"
grep -F 'MultipleAwaits.pipe:31' "$proof/units/await-debugger.output"
grep -Fx '$1 = 1' "$proof/units/await-debugger.output"
grep -Fx '$2 = 40' "$proof/units/await-debugger.output"
grep -Fx '$3 = 39' "$proof/units/await-debugger.output"
grep -F 'exited normally' "$proof/units/await-debugger.output"
unit reference-multiple-awaits /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/await-reference" "$await_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.MultipleAwaits "$assets/scripts/verify-multiple-awaits.sh" --require-suspension
mkdir "$proof/async-calls"
async_call_source="$assets/AsyncCalls.pipe"
async_call_app="$proof/async-calls/async-calls"
unit build-async-calls /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$async_call_source" NativeProof.AsyncCalls.App.Run "$async_call_app"
unit run-async-calls /bin/bash "$assets/scripts/verify-async-calls.sh" "$async_call_app" --require-two-suspensions
mkdir "$proof/async-calls-release"
async_call_release="$proof/async-calls-release/async-calls"
unit build-async-calls-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$async_call_source" NativeProof.AsyncCalls.App.Run "$async_call_release" release
unit run-async-calls-release /bin/bash "$assets/scripts/verify-async-calls.sh" "$async_call_release" --require-two-suspensions
unit async-calls-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break AsyncCalls.pipe:83' -ex 'run 39 --require-two-suspensions' \
    -ex 'print input' -ex 'print delta' -ex 'continue' "$async_call_app"
grep -F 'AsyncCalls.pipe:83' "$proof/units/async-calls-debugger.output"
grep -Fx '$1 = 41' "$proof/units/async-calls-debugger.output"
grep -Fx '$2 = 1' "$proof/units/async-calls-debugger.output"
grep -F 'exited normally' "$proof/units/async-calls-debugger.output"
unit reference-async-calls /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/async-call-reference" "$async_call_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.AsyncCalls "$assets/scripts/verify-async-calls.sh" --require-suspension
mkdir "$proof/awaited-finally" "$proof/awaited-finally-release"
finally_source="$assets/AwaitedFinally.pipe"
finally_app="$proof/awaited-finally/awaited-finally"
finally_release="$proof/awaited-finally-release/awaited-finally"
unit build-awaited-finally /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$finally_source" NativeProof.AwaitedFinally.App.Run "$finally_app"
unit run-awaited-finally /bin/bash "$assets/scripts/verify-awaited-finally.sh" "$finally_app" --require-two-suspensions
unit build-awaited-finally-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$finally_source" NativeProof.AwaitedFinally.App.Run "$finally_release" release
unit run-awaited-finally-release /bin/bash "$assets/scripts/verify-awaited-finally.sh" \
    "$finally_release" --require-two-suspensions
unit awaited-finally-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break AwaitedFinally.pipe:72' -ex 'run 0 --require-two-suspensions' \
    -ex 'print input' -ex 'print completed.value' -ex continue "$finally_app"
grep -F 'AwaitedFinally.pipe:72' "$proof/units/awaited-finally-debugger.output"
grep -Fx '$1 = 0' "$proof/units/awaited-finally-debugger.output"
grep -Fx '$2 = 2' "$proof/units/awaited-finally-debugger.output"
grep -F 'exited normally' "$proof/units/awaited-finally-debugger.output"
unit reference-awaited-finally /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/awaited-finally-reference" "$finally_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.AwaitedFinally "$assets/scripts/verify-awaited-finally.sh" --require-suspension
mkdir "$proof/native-catch" "$proof/native-catch-release"
catch_source="$assets/NativeCatch.pipe"
catch_app="$proof/native-catch/native-catch"
catch_release="$proof/native-catch-release/native-catch"
unit build-native-catch /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$catch_source" NativeProof.NativeCatch.App.Run "$catch_app"
unit run-native-catch /bin/bash "$assets/scripts/verify-native-catch.sh" "$catch_app" --require-two-suspensions
unit build-native-catch-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$catch_source" NativeProof.NativeCatch.App.Run "$catch_release" release
unit run-native-catch-release /bin/bash "$assets/scripts/verify-native-catch.sh" "$catch_release" --require-two-suspensions
unit native-catch-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break NativeCatch.pipe:33' -ex 'run 0 --require-two-suspensions' \
    -ex 'whatis caught' -ex 'print caught.size' -ex 'print recovered.value' -ex continue "$catch_app"
grep -F 'NativeCatch.pipe:33' "$proof/units/native-catch-debugger.output"
grep -E '^type = (struct )?Error$' "$proof/units/native-catch-debugger.output"
grep -Fx '$1 = 15' "$proof/units/native-catch-debugger.output"
grep -Fx '$2 = 42' "$proof/units/native-catch-debugger.output"
grep -F 'exited with code 01' "$proof/units/native-catch-debugger.output"
unit reference-native-catch /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/native-catch-reference" "$catch_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.NativeCatch "$assets/scripts/verify-native-catch.sh" --require-suspension
# A separately selected hosted method calls the primary through a source continuation.
for profile in debug release; do
    unit "hosted-primary-source-call-$profile" "$compiler" \
        --source "$PWD/tests/native-models/IndependentTokens.pipe" --entry IndependentTokens.App.Direct \
        --host-symbol proof_hosted_independent_tokens --host-method Local --profile "$profile" \
        --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/hosted-primary-source-call-$profile.o"
done

mkdir "$proof/independent-cancellation" "$proof/independent-cancellation-release"
token_source="$assets/IndependentCancellation.pipe"
token_app="$proof/independent-cancellation/independent-cancellation"
token_release="$proof/independent-cancellation-release/independent-cancellation"
unit build-independent-cancellation /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$token_source" NativeProof.IndependentCancellation.App.Run "$token_app"
unit run-independent-cancellation /bin/bash "$assets/scripts/verify-independent-cancellation.sh" \
    "$token_app" --require-two-suspensions
unit build-independent-cancellation-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$token_source" NativeProof.IndependentCancellation.App.Run "$token_release" release
unit run-independent-cancellation-release /bin/bash "$assets/scripts/verify-independent-cancellation.sh" \
    "$token_release" --require-two-suspensions
unit independent-cancellation-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break IndependentCancellation.pipe:57' -ex 'run 0 --require-two-suspensions' \
    -ex 'print bodyToken == cleanupToken' -ex 'print scenario' -ex continue "$token_app"
grep -F 'IndependentCancellation.pipe:57' "$proof/units/independent-cancellation-debugger.output"
grep -Ex '\$1 = (0|false)' "$proof/units/independent-cancellation-debugger.output"
grep -Fx '$2 = 0' "$proof/units/independent-cancellation-debugger.output"
grep -F 'exited normally' "$proof/units/independent-cancellation-debugger.output"
unit reference-independent-cancellation /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/independent-cancellation-reference" "$token_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.IndependentCancellation "$assets/scripts/verify-independent-cancellation.sh" --require-two-suspensions
mkdir "$proof/single-cancellation-token"
single_token_app="$proof/single-cancellation-token/single-cancellation-token"
unit build-single-cancellation-token /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$token_source" NativeProof.IndependentCancellation.SingleToken.Run "$single_token_app"
unit run-single-cancellation-token /bin/bash -e -c '
    test "$(env -i "$1" 42 --require-suspension)" = 42
    status=0
    output=$(env -i "$1" 42 --cancel-body-after-suspension 2>&1) || status=$?
    test "$status" -eq 2
    test "$output" = "operation cancelled"
' -- "$single_token_app"
for profile in debug release; do
    token_only_app="$proof/token-only-$profile"
    unit "build-token-only-$profile" /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$token_source" NativeProof.IndependentCancellation.TokenOnly.Run \
        "$token_only_app" "$profile"
    unit "run-token-only-$profile" /bin/bash -e -c '
        test "$(env -i "$1" --require-two-suspensions)" = 42
        for option in --cancel-body-after-suspension --cancel-cleanup-after-second-suspension; do
            status=0
            output=$(env -i "$1" "$option" 2>&1) || status=$?
            test "$status" -eq 2
            test "$output" = "operation cancelled"
        done
    ' -- "$token_only_app"
done
mkdir "$proof/structured-results" "$proof/structured-results-release"
structured_source="$assets/StructuredResults.pipe"
structured_app="$proof/structured-results/structured-results"
structured_release="$proof/structured-results-release/structured-results"
unit build-structured-results /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$structured_source" NativeProof.StructuredResults.App.Run "$structured_app"
unit run-structured-results /bin/bash "$assets/scripts/verify-structured-results.sh" \
    "$structured_app" --require-two-suspensions
unit build-structured-results-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$structured_source" NativeProof.StructuredResults.App.Run "$structured_release" release
unit run-structured-results-release /bin/bash "$assets/scripts/verify-structured-results.sh" \
    "$structured_release" --require-two-suspensions
unit structured-results-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break StructuredResults.pipe:31' -ex 'run 0 --require-two-suspensions' \
    -ex 'print report.heading.source' -ex 'print report.heading.accepted' -ex 'print report.score' -ex continue "$structured_app"
grep -F 'StructuredResults.pipe:31' "$proof/units/structured-results-debugger.output"
grep -Fx '$1 = 17' "$proof/units/structured-results-debugger.output"
grep -Fx '$2 = true' "$proof/units/structured-results-debugger.output"
grep -Fx '$3 = 42' "$proof/units/structured-results-debugger.output"
grep -F 'exited normally' "$proof/units/structured-results-debugger.output"
unit reference-structured-results /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/structured-results-reference" "$structured_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.StructuredResults "$assets/scripts/verify-structured-results.sh" --require-two-suspensions
mkdir "$proof/owned-list-results" "$proof/owned-list-results-release"
owned_list_source="$assets/OwnedListResults.pipe"
owned_list_app="$proof/owned-list-results/owned-list-results"
owned_list_release="$proof/owned-list-results-release/owned-list-results"
unit build-owned-list-results /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$owned_list_source" NativeProof.OwnedListResults.App.Run "$owned_list_app"
unit run-owned-list-results /bin/bash "$assets/scripts/verify-owned-list-results.sh" \
    "$owned_list_app" --require-two-suspensions
unit build-owned-list-results-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$owned_list_source" NativeProof.OwnedListResults.App.Run "$owned_list_release" release
unit run-owned-list-results-release /bin/bash "$assets/scripts/verify-owned-list-results.sh" \
    "$owned_list_release" --require-two-suspensions
unit owned-list-results-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break OwnedListResults.pipe:29' -ex 'run 0 --require-two-suspensions' \
    -ex 'print report.items.count' -ex 'print report.original.count' -ex 'print owned.count' -ex continue "$owned_list_app"
grep -F 'OwnedListResults.pipe:29' "$proof/units/owned-list-results-debugger.output"
grep -Fx '$1 = 3' "$proof/units/owned-list-results-debugger.output"
grep -Fx '$2 = 1' "$proof/units/owned-list-results-debugger.output"
grep -Fx '$3 = 4' "$proof/units/owned-list-results-debugger.output"
grep -F 'exited normally' "$proof/units/owned-list-results-debugger.output"
unit reference-owned-list-results /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/owned-list-results-reference" "$owned_list_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.OwnedListResults "$assets/scripts/verify-owned-list-results.sh" --require-two-suspensions
mkdir "$proof/optional-enum-results" "$proof/optional-enum-results-release"
optional_enum_source="$assets/OptionalEnumResults.pipe"
optional_enum_app="$proof/optional-enum-results/optional-enum-results"
optional_enum_release="$proof/optional-enum-results-release/optional-enum-results"
unit build-optional-enum-results /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$optional_enum_source" NativeProof.OptionalEnumResults.App.Run "$optional_enum_app"
unit run-optional-enum-results /bin/bash "$assets/scripts/verify-optional-enum-results.sh" \
    "$optional_enum_app" --require-two-suspensions
unit build-optional-enum-results-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$optional_enum_source" NativeProof.OptionalEnumResults.App.Run "$optional_enum_release" release
unit run-optional-enum-results-release /bin/bash "$assets/scripts/verify-optional-enum-results.sh" \
    "$optional_enum_release" --require-two-suspensions
unit optional-enum-results-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break OptionalEnumResults.pipe:32' -ex 'run 0 --require-two-suspensions' \
    -ex 'print second.outcome' -ex 'print second.failure.hasValue' \
    -ex 'print second.failure.value.code' -ex continue "$optional_enum_app"
grep -F 'OptionalEnumResults.pipe:32' "$proof/units/optional-enum-results-debugger.output"
grep -E '^\$1 = .*Failed$' "$proof/units/optional-enum-results-debugger.output"
grep -Fx '$2 = true' "$proof/units/optional-enum-results-debugger.output"
grep -Fx '$3 = 42' "$proof/units/optional-enum-results-debugger.output"
grep -F 'exited normally' "$proof/units/optional-enum-results-debugger.output"
unit reference-optional-enum-results /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/optional-enum-results-reference" "$optional_enum_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.OptionalEnumResults "$assets/scripts/verify-optional-enum-results.sh" --require-two-suspensions
typed_failure_source="$assets/TypedFailureResults.pipe"
typed_failure_app="$proof/typed-failure-results/typed-failure-results"
typed_failure_release="$proof/typed-failure-results-release/typed-failure-results"
typed_failure_entry=NativeProof.TypedFailureResults.App.Run
mkdir "$(dirname "$typed_failure_app")" "$(dirname "$typed_failure_release")"
unit build-typed-failure-results /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$typed_failure_source" "$typed_failure_entry" "$typed_failure_app"
unit run-typed-failure-results /bin/bash "$assets/scripts/verify-typed-failure-results.sh" \
    "$typed_failure_app" --require-two-suspensions
unit build-typed-failure-results-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$typed_failure_source" "$typed_failure_entry" "$typed_failure_release" release
unit run-typed-failure-results-release /bin/bash "$assets/scripts/verify-typed-failure-results.sh" \
    "$typed_failure_release" --require-two-suspensions
unit typed-failure-results-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break TypedFailureResults.pipe:24' -ex 'run 0 --require-two-suspensions' \
    -ex 'print failed.tag' -ex 'print failed.error.code' -ex 'print failed.error.cleanup.tag' \
    -ex continue "$typed_failure_app"
typed_failure_debug_log="$proof/units/typed-failure-results-debugger.output"
grep -F 'TypedFailureResults.pipe:24' "$typed_failure_debug_log"
grep -Fx '$1 = 2' "$typed_failure_debug_log"
grep -Fx '$2 = 42' "$typed_failure_debug_log"
grep -Fx '$3 = 1' "$typed_failure_debug_log"
grep -F 'exited normally' "$typed_failure_debug_log"
unit reference-typed-failure-results /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/typed-failure-results-reference" "$typed_failure_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.TypedFailureResults "$assets/scripts/verify-typed-failure-results.sh" --require-two-suspensions
synchronous_source="$assets/SynchronousValues.pipe"
synchronous_entry=NativeProof.SynchronousValues.App.Run
synchronous_verifier="$assets/scripts/verify-synchronous-values.sh"
mkdir "$proof/synchronous-values" "$proof/synchronous-values-release"
synchronous_app="$proof/synchronous-values/synchronous-values"
synchronous_release="$proof/synchronous-values-release/synchronous-values"
unit build-synchronous-values /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$synchronous_source" "$synchronous_entry" "$synchronous_app"
unit run-synchronous-values /bin/bash "$synchronous_verifier" "$synchronous_app" --require-two-suspensions
unit build-synchronous-values-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$synchronous_source" "$synchronous_entry" "$synchronous_release" release
unit run-synchronous-values-release /bin/bash "$synchronous_verifier" "$synchronous_release" --require-two-suspensions
unit synchronous-values-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break SynchronousValues.pipe:58' -ex 'run 0 --require-two-suspensions' \
    -ex 'print request.divisor' -ex 'print reject' -ex bt -ex continue "$synchronous_app"
synchronous_debug_log="$proof/units/synchronous-values-debugger.output"
grep -E '^#0 .*Rule\.Evaluate.* at SynchronousValues\.pipe:58$' "$synchronous_debug_log"
grep -E '^#1 .*App\.Run.* at SynchronousValues\.pipe:22$' "$synchronous_debug_log"
grep -Fx '$1 = 2' "$synchronous_debug_log"
grep -Fx '$2 = false' "$synchronous_debug_log"
grep -F 'exited normally' "$synchronous_debug_log"
unit reference-synchronous-values /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/synchronous-values-reference" "$synchronous_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.SynchronousValues "$synchronous_verifier" --require-two-suspensions
propagation_source="$assets/NativePropagation.pipe"
propagation_entry=NativeProof.NativePropagation.App.Run
propagation_verifier="$assets/scripts/verify-native-propagation.sh"
mkdir "$proof/native-propagation" "$proof/native-propagation-release"
propagation_app="$proof/native-propagation/native-propagation"
propagation_release="$proof/native-propagation-release/native-propagation"
unit build-native-propagation /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$propagation_source" "$propagation_entry" "$propagation_app"
unit run-native-propagation /bin/bash "$propagation_verifier" "$propagation_app" --require-two-suspensions
unit build-native-propagation-release /bin/bash "$assets/scripts/build-runtime-app.sh" "$proof/runtime-sdk" \
    "$PIPELANG_NATIVE_DEBUG_LLC" "$propagation_source" "$propagation_entry" "$propagation_release" release
unit run-native-propagation-release /bin/bash "$propagation_verifier" "$propagation_release" --require-two-suspensions
unit native-propagation-debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'break NativePropagation.pipe:30' -ex 'run 1 --require-two-suspensions' \
    -ex 'print scenario' -ex continue "$propagation_app"
propagation_debug_log="$proof/units/native-propagation-debugger.output"
grep -E '^Breakpoint 1, .*App\.Run .* at NativePropagation\.pipe:30$' "$propagation_debug_log"
grep -Fx '$1 = 1' "$propagation_debug_log"
grep -Fx 'source reading failed' "$propagation_debug_log"
grep -F 'exited with code 01' "$propagation_debug_log"
unit reference-native-propagation /bin/bash "$assets/scripts/verify-async-reference.sh" \
    "$build/model-compiler/pipelang-modelc" "$build/native-app-runtime-compiler" \
    "$proof/native-propagation-reference" "$propagation_source" "$PWD/sdk/pipeview/model/native/include" \
    NativeProof.NativePropagation "$propagation_verifier" --require-two-suspensions
unit async-runtime-sanitizers /bin/bash "$assets/scripts/verify-runtime-sanitizers.sh" \
    "$compiler" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native" "$proof/async-runtime-sanitizers"
unit authored-address-sanitizer /bin/bash "$assets/scripts/verify-authored-asan.sh" \
    "$compiler" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native/include" "$build/libpipelang_native_app_runtime.a" "$proof/authored-asan"
unit authored-thread-sanitizer /bin/bash "$assets/scripts/verify-authored-tsan.sh" \
    "$compiler" "$build/native-app-runtime-compiler" "$PIPELANG_NATIVE_DEBUG_LLC" \
    "$PWD/sdk/pipeview/model/native" "$proof/authored-tsan" "$tsan_archive" "$tsan_archive_sha256"
mkdir "$proof/macos-runtime"
unit macos-runtime-object "$compiler" --source "$suspending_source" --entry SuspendingCollections.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/collections-async.o"
"$llvm_bin/llvm-readobj" --file-headers --symbols "$proof/macos-runtime/collections-async.o" \
    > "$proof/macos-runtime/headers.txt"
grep -Fx 'Format: Mach-O arm64' "$proof/macos-runtime/headers.txt"
grep -F 'pipelang_native_v19_entry' "$proof/macos-runtime/headers.txt" >/dev/null
unit macos-owned-worker-object "$compiler" --source "$owned_source" --entry NativeProof.OwnedWorker.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/owned-worker.o"
unit macos-result-object "$compiler" --source "$result_source" --entry NativeProof.ResultOutcomes.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/result-outcomes.o"
unit macos-loop-object "$compiler" --source "$loop_source" --entry NativeProof.LoopOutcomes.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/loop-outcomes.o"
unit macos-await-object "$compiler" --source "$await_source" --entry NativeProof.MultipleAwaits.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/multiple-awaits.o"
unit macos-async-calls-object "$compiler" --source "$async_call_source" --entry NativeProof.AsyncCalls.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/async-calls.o"
unit macos-awaited-finally-object "$compiler" --source "$finally_source" --entry NativeProof.AwaitedFinally.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/awaited-finally.o"
unit macos-native-catch-object "$compiler" --source "$catch_source" --entry NativeProof.NativeCatch.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/native-catch.o"
unit macos-independent-cancellation-object "$compiler" --source "$token_source" --entry NativeProof.IndependentCancellation.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/independent-cancellation.o"
unit macos-structured-results-object "$compiler" --source "$structured_source" --entry NativeProof.StructuredResults.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/structured-results.o"
unit macos-owned-list-results-object "$compiler" --source "$owned_list_source" --entry NativeProof.OwnedListResults.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/owned-list-results.o"
unit macos-optional-enum-results-object "$compiler" --source "$optional_enum_source" --entry NativeProof.OptionalEnumResults.App.Run \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/optional-enum-results.o"

unit macos-typed-failure-results-object "$compiler" --source "$typed_failure_source" --entry "$typed_failure_entry" \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/typed-failure-results.o"
unit macos-synchronous-values-object "$compiler" --source "$synchronous_source" --entry "$synchronous_entry" \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/synchronous-values.o"
unit macos-native-propagation-object "$compiler" --source "$propagation_source" --entry "$propagation_entry" \
    --target macos-arm64 --runtime --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/native-propagation.o"
unit macos-hosted-instance-object "$compiler" --source "$assets/HostedInstance.pipe" \
    --entry NativeProof.HostedInstance.Session.Run --host-symbol proof_hosted_session \
    --target macos-arm64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/hosted-instance.o"
unit macos-hosted-report-object "$compiler" --source "$assets/HostedReport.pipe" \
    --entry NativeProof.HostedReport.Session.Run --host-symbol proof_hosted_report \
    --target macos-arm64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/hosted-report.o"
unit macos-hosted-clock-object "$compiler" --source "$assets/HostedClock.pipe" \
    --entry NativeProof.HostedClock.Session.Run --host-symbol proof_hosted_clock \
    --target macos-arm64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/hosted-clock.o"
unit macos-hosted-receivers-object "$compiler" --source "$assets/HostedReceivers.pipe" \
    --entry NativeProof.HostedReceivers.Session.Run --host-symbol proof_hosted_receivers \
    --target macos-arm64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/hosted-receivers.o"
unit macos-interface-dispatch-object "$compiler" --source "$assets/InterfaceDispatch.pipe" \
    --entry NativeProof.InterfaceDispatch.App.Run --runtime \
    --target macos-arm64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/macos-runtime/interface-dispatch.o"

if [[ -n "$windows_tools" ]]; then
    mkdir "$proof/windows-runtime" "$proof/windows-runtime/standalone"
    unit stage-windows-runtime-sdk /bin/bash "$assets/scripts/stage-windows-sdk.sh" \
        "$compiler" "$windows_tools" "$PWD" "$proof/windows-runtime/sdk" "$build/sdk-tools/pipelang-sdk-tool"
    windows_app="$proof/windows-runtime/standalone/collections-async.exe"
    unit build-windows-runtime-app /usr/bin/strace -qq -f -e trace=execve,execveat \
        -o "$proof/windows-runtime/build.exec" /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$suspending_source" SuspendingCollections.App.Run "$windows_app" "$proof/windows-runtime/collections-async.pdb"
    if grep -E 'execve\("[^"]*/(g\+\+|c\+\+|clang(\+\+)?|cc1plus|[^"/]+-clang\+\+)(-[0-9]+)?"' "$proof/windows-runtime/build.exec"; then
        printf '%s\n' 'The Windows application build invoked a C++ compiler' >&2
        exit 1
    fi
    "$windows_tools/bin/llvm-readobj" --coff-imports "$windows_app" > "$proof/windows-runtime/imports.txt"
    "$windows_tools/bin/llvm-readobj" --coff-debug-directory "$windows_app" > "$proof/windows-runtime/debug-directory.txt"
    grep -E '^[[:space:]]*PDBFileName: collections-async\.pdb$' "$proof/windows-runtime/debug-directory.txt"
    unit verify-windows-executable-publication /bin/bash "$assets/scripts/verify-windows-executable-publication.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$suspending_source" "$proof/windows-runtime"
    imports=$(awk '$1 == "Name:" {print tolower($2)}' "$proof/windows-runtime/imports.txt")
    test -n "$imports"
    if grep -Eiv '^(kernel32|advapi32|shell32|user32|ucrtbase|msvcrt|api-ms-win-crt-[a-z0-9-]+)\.dll$' <<< "$imports"; then
        printf '%s\n' 'The Windows application imports a library outside the Windows system runtimes' >&2
        exit 1
    fi
    unit build-windows-process-arguments /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$process_arguments_source" "$process_arguments_entry" "$proof/windows-runtime/standalone/process-arguments.exe" \
        "$proof/windows-runtime/process-arguments.pdb"
    unit build-windows-owned-worker /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$owned_source" NativeProof.OwnedWorker.App.Run "$proof/windows-runtime/standalone/owned-worker.exe" \
        "$proof/windows-runtime/owned-worker.pdb"
    unit build-windows-result-outcomes /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$result_source" NativeProof.ResultOutcomes.App.Run "$proof/windows-runtime/standalone/result-outcomes.exe" \
        "$proof/windows-runtime/result-outcomes.pdb"
    unit build-windows-loop-outcomes /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$loop_source" NativeProof.LoopOutcomes.App.Run "$proof/windows-runtime/standalone/loop-outcomes.exe" \
        "$proof/windows-runtime/loop-outcomes.pdb"
    unit build-windows-await-app /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$await_source" NativeProof.MultipleAwaits.App.Run "$proof/windows-runtime/standalone/multiple-awaits.exe" \
        "$proof/windows-runtime/multiple-awaits.pdb"
    unit build-windows-async-calls /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$async_call_source" NativeProof.AsyncCalls.App.Run "$proof/windows-runtime/standalone/async-calls.exe" \
        "$proof/windows-runtime/async-calls.pdb"
    unit build-windows-awaited-finally /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$finally_source" NativeProof.AwaitedFinally.App.Run "$proof/windows-runtime/standalone/awaited-finally.exe" \
        "$proof/windows-runtime/awaited-finally.pdb"
    unit build-windows-native-catch /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$catch_source" NativeProof.NativeCatch.App.Run "$proof/windows-runtime/standalone/native-catch.exe" \
        "$proof/windows-runtime/native-catch.pdb"
    unit build-windows-independent-cancellation /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$token_source" NativeProof.IndependentCancellation.App.Run \
        "$proof/windows-runtime/standalone/independent-cancellation.exe" "$proof/windows-runtime/independent-cancellation.pdb"
    unit build-windows-structured-results /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$structured_source" NativeProof.StructuredResults.App.Run "$proof/windows-runtime/standalone/structured-results.exe" \
        "$proof/windows-runtime/structured-results.pdb"
    unit build-windows-owned-list-results /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$owned_list_source" NativeProof.OwnedListResults.App.Run \
        "$proof/windows-runtime/standalone/owned-list-results.exe" "$proof/windows-runtime/owned-list-results.pdb"
    unit build-windows-optional-enum-results /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$optional_enum_source" NativeProof.OptionalEnumResults.App.Run \
        "$proof/windows-runtime/standalone/optional-enum-results.exe" "$proof/windows-runtime/optional-enum-results.pdb"
    unit build-windows-typed-failure-results /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$typed_failure_source" "$typed_failure_entry" "$proof/windows-runtime/standalone/typed-failure-results.exe" \
        "$proof/windows-runtime/typed-failure-results.pdb"
    unit build-windows-synchronous-values /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$synchronous_source" "$synchronous_entry" "$proof/windows-runtime/standalone/synchronous-values.exe" \
        "$proof/windows-runtime/synchronous-values.pdb"
    mkdir "$proof/windows-runtime/repeat"
    unit repeat-windows-synchronous-values /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$synchronous_source" "$synchronous_entry" "$proof/windows-runtime/repeat/synchronous-values.exe" \
        "$proof/windows-runtime/repeat/synchronous-values.pdb"
    cmp "$proof/windows-runtime/standalone/synchronous-values.exe" "$proof/windows-runtime/repeat/synchronous-values.exe"
    cmp "$proof/windows-runtime/synchronous-values.pdb" "$proof/windows-runtime/repeat/synchronous-values.pdb"
    unit build-windows-native-propagation /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$propagation_source" "$propagation_entry" "$proof/windows-runtime/standalone/native-propagation.exe" \
        "$proof/windows-runtime/native-propagation.pdb"
    unit windows-hosted-instance-object "$compiler" --source "$assets/HostedInstance.pipe" \
        --entry NativeProof.HostedInstance.Session.Run --host-symbol proof_hosted_session \
        --target windows-x64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/windows-runtime/hosted-instance.obj"
    unit windows-hosted-report-object "$compiler" --source "$assets/HostedReport.pipe" \
        --entry NativeProof.HostedReport.Session.Run --host-symbol proof_hosted_report \
        --target windows-x64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/windows-runtime/hosted-report.obj"
    unit windows-hosted-clock-object "$compiler" --source "$assets/HostedClock.pipe" \
        --entry NativeProof.HostedClock.Session.Run --host-symbol proof_hosted_clock \
        --target windows-x64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/windows-runtime/hosted-clock.obj"
    unit link-windows-hosted-instance "$windows_tools/bin/x86_64-w64-mingw32-clang++" \
        -std=c++20 -O2 -static -pthread -Wall -Wextra -Wpedantic -Werror \
        -I "$PWD/sdk/pipeview/model/native/include" "$assets/HostedInstance.cpp" \
        "$proof/windows-runtime/hosted-instance.obj" "$proof/windows-runtime/hosted-report.obj" \
        "$proof/windows-runtime/hosted-clock.obj" \
        "$proof/windows-runtime/sdk/link/libpipelang_native_app_runtime.a" \
        -o "$proof/windows-runtime/standalone/hosted-instance.exe"
    unit windows-hosted-receivers-object "$compiler" --source "$assets/HostedReceivers.pipe" \
        --entry NativeProof.HostedReceivers.Session.Run --host-symbol proof_hosted_receivers \
        --target windows-x64 --llc "$PIPELANG_NATIVE_DEBUG_LLC" --output "$proof/windows-runtime/hosted-receivers.obj"
    unit link-windows-hosted-receivers "$windows_tools/bin/x86_64-w64-mingw32-clang++" \
        -std=c++20 -O2 -static -pthread -Wall -Wextra -Wpedantic -Werror \
        -I "$PWD/sdk/pipeview/model/native/include" "$assets/HostedReceivers.cpp" \
        "$proof/windows-runtime/hosted-receivers.obj" "$proof/windows-runtime/sdk/link/libpipelang_native_app_runtime.a" \
        -o "$proof/windows-runtime/standalone/hosted-receivers.exe"
    unit build-windows-interface-dispatch /bin/bash "$assets/scripts/build-windows-app.sh" \
        "$proof/windows-runtime/sdk" "$windows_tools" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$assets/InterfaceDispatch.pipe" NativeProof.InterfaceDispatch.App.Run \
        "$proof/windows-runtime/standalone/interface-dispatch.exe" "$proof/windows-runtime/interface-dispatch.pdb"
    mkdir "$proof/windows-arm64-runtime"
    unit stage-windows-arm64-sdk /bin/bash "$assets/scripts/stage-windows-sdk.sh" \
        "$compiler" "$windows_tools" "$PWD" "$proof/windows-arm64-runtime/sdk" \
        "$build/sdk-tools/pipelang-sdk-tool" aarch64
    unit build-windows-arm64-runtime-app /bin/bash "$assets/scripts/verify-windows-arm64-app.sh" \
        "$proof/windows-arm64-runtime/sdk" "$proof/windows-runtime/sdk" "$windows_tools" \
        "$PIPELANG_NATIVE_DEBUG_LLC" "$suspending_source" "$proof/windows-arm64-runtime"
fi
unit cross-object-matrix /bin/bash "$assets/scripts/cross-objects.sh" "$compiler" "$proof" "$PIPELANG_NATIVE_DEBUG_LLC"
if [[ -n "$linker_bin" ]]; then
    unit cross-link-matrix /bin/bash "$assets/scripts/link-targets.sh" \
        "$proof" "$PIPELANG_NATIVE_DEBUG_LLC" "$linker_bin" "$qemu_arm64"
    unit macos-application-link /bin/bash "$assets/scripts/verify-macos-application-link.sh" \
        "$compiler" "$build/sdk-tools/pipelang-sdk-tool" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$linker_bin/ld64.lld" "$proof/macos-application-link" arm64
    unit macos-x64-application-link /bin/bash "$assets/scripts/verify-macos-application-link.sh" \
        "$compiler" "$build/sdk-tools/pipelang-sdk-tool" "$PIPELANG_NATIVE_DEBUG_LLC" \
        "$linker_bin/ld64.lld" "$proof/macos-x64-application-link" x86_64
fi

printf '%s\n' 'public Class Invalid { public static pure int Select(int input) => true; }' > "$proof/Invalid.pipe"
printf '%s\n' 'public Class Unsupported { public static pure int Select(int input) => input + 1; }' > "$proof/Unsupported.pipe"
unit fail-closed-check /bin/bash "$assets/scripts/fail-closed.sh" "$compiler" "$proof" "$PIPELANG_NATIVE_DEBUG_LLC"
unit debugger "$PIPELANG_NATIVE_DEBUG_GDB" --batch --nx --quiet \
    -ex "directory $proof" -ex 'set disable-randomization off' \
    -ex 'break Select.pipe:5' -ex run -ex 'disable 1' -ex 'print result' -ex 'print accepted' \
    -ex next -ex next -ex 'print result' -ex bt -ex continue "$proof/select"
debug_log="$proof/units/debugger.output"
grep -F 'Select.pipe:5' "$debug_log"
grep -Fx '$1 = 7' "$debug_log"
grep -Fx '$2 = true' "$debug_log"
grep -Fx '$3 = 42' "$debug_log"
grep -E '^#0 .* at (.*[/])?Select\.pipe:9$' "$debug_log"
grep -F 'exited normally' "$debug_log"
sha256sum --check --status "$proof/inputs.sha256"

write_debug_workspace() {
    local name=$1
    local source=$2
    local program=$3
    jq -n --arg name "Debug PipeLang $name" --arg source "$(dirname "$source")" --arg program "$program" \
        --arg gdb "$PIPELANG_NATIVE_DEBUG_GDB" '{
    folders: [{path: $source}],
    settings: {"debug.allowBreakpointsEverywhere": true},
    launch: {
        version: "0.2.0",
        configurations: [{
            name: $name,
            type: "cppdbg",
            request: "launch",
            program: $program,
            cwd: $source,
            MIMode: "gdb",
            miDebuggerPath: $gdb,
            "externalConsole": false,
            setupCommands: [
                {text: "-gdb-set disable-randomization off", ignoreFailures: false}
            ]
        }]
    }
}' > "$proof/$name.code-workspace"
}

write_debug_workspace Select "$source_path" "$proof/select"
write_debug_workspace Standalone "$standalone_source" "$proof/standalone/synchronous-debug"
printf 'Native debug proof: %s\n' "$proof"
