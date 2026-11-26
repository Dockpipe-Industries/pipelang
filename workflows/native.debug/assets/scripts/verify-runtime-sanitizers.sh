#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
compiler_record=$2
llc=$3
runtime=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
mkdir "$proof"
runtime_compile_options=()
if [[ -f "$compiler_record-id" && $(<"$compiler_record-id") == GNU ]]; then
    runtime_compile_options+=("@${runtime%/native}/compiler-profiles/gnu-conservative-gc.rsp")
fi

# App code still comes from the native compiler. This validation link
# instruments the C++ SDK bridge and includes the sanitizer support libraries.
# Keep the largest runtime units and their line information within the compiler memory limits.
for unit in native_app_runtime native_app_launcher native_host native_host_value \
    native_composition NativeAtomics NativeBytes NativeConcurrentDictionaries NativeConcurrentSequences \
    NativeDictionaries NativeFloating NativeParallel NativeReceiverStorage NativeSequences; do
    optimization=-O1
    compile_options=()
    if [[ "$unit" == native_app_runtime || "$unit" == native_host ]]; then
        optimization=-O0
        compile_options=("${runtime_compile_options[@]}")
    fi
    "$cxx" -std=c++20 "$optimization" -g1 -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
        "${compile_options[@]}" -I "$runtime/include" -c "$runtime/src/$unit.cpp" -o "$proof/$unit.o"
done
runtime_objects=("$proof/native_app_runtime.o" "$proof/native_host_value.o" "$proof/NativeAtomics.o" "$proof/NativeBytes.o"
    "$proof/NativeConcurrentDictionaries.o" "$proof/NativeConcurrentSequences.o"
    "$proof/NativeDictionaries.o" "$proof/NativeFloating.o" "$proof/NativeParallel.o" "$proof/NativeReceiverStorage.o"
    "$proof/NativeSequences.o")

verify_fixture() {
    local source=$1
    local name=$2
    local build="$proof/$name"
    mkdir "$build"
    "$compiler" --source "$assets/$source.pipe" --entry "NativeProof.$source.App.Run" \
        --runtime --llc "$llc" --output "$build/app.o"
    "$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
        "$proof/native_app_launcher.o" "$build/app.o" -o "$build/app"
    /bin/bash "$assets/scripts/verify-$name.sh" "$build/app" --require-two-suspensions
}

verify_fixture AsyncCalls async-calls
verify_fixture AwaitedFinally awaited-finally
verify_fixture NativeCatch native-catch
verify_fixture IndependentCancellation independent-cancellation
verify_fixture StructuredResults structured-results
verify_fixture OwnedListResults owned-list-results
verify_fixture OptionalEnumResults optional-enum-results
verify_fixture TypedFailureResults typed-failure-results
verify_fixture SynchronousValues synchronous-values
verify_fixture NativePropagation native-propagation
verify_fixture InterfaceDispatch interface-dispatch
verify_fixture HashSets hash-sets
verify_fixture TextOperations text-operations

"$compiler" --source "$assets/UInt32Values.pipe" --entry NativeProof.UInt32Values.App.Run \
    --runtime --llc "$llc" --output "$proof/uint32-values.o"
"$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
    "$proof/native_app_launcher.o" "$proof/uint32-values.o" -o "$proof/uint32-values"
test "$(env -i "$proof/uint32-values" 0 --require-suspension)" = 4294967295

"$compiler" --source "$assets/FloatingValues.pipe" --entry NativeProof.FloatingValues.App.Run \
    --runtime --llc "$llc" --output "$proof/floating-values.o"
"$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
    "$proof/native_app_launcher.o" "$proof/floating-values.o" -o "$proof/floating-values"
test "$(env -i "$proof/floating-values" 0 --require-suspension)" = 42

"$compiler" --source "$assets/ByteBuffers.pipe" --entry NativeProof.ByteBuffers.App.Run \
    --runtime --llc "$llc" --output "$proof/byte-buffers.o"
"$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
    "$proof/native_app_launcher.o" "$proof/byte-buffers.o" -o "$proof/byte-buffers"
test "$(env -i "$proof/byte-buffers" 0 --require-suspension)" = 42

"$compiler" --source "$assets/ByteBuffers.pipe" --entry NativeProof.ByteBuffers.Session.Run \
    --host-symbol proof_byte_session --llc "$llc" --output "$proof/hosted-bytes.o"
"$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/ByteBuffersHosted.cpp" "$proof/hosted-bytes.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/hosted-bytes"
env -i "$proof/hosted-bytes"

"$compiler" --source "$assets/Sequences.pipe" --entry NativeProof.Sequences.App.Run \
    --runtime --llc "$llc" --output "$proof/sequences.o"
"$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
    "$proof/native_app_launcher.o" "$proof/sequences.o" -o "$proof/sequences"
test "$(env -i "$proof/sequences" 0 --require-two-suspensions)" = 42

"$compiler" --source "$assets/Sequences.pipe" --entry NativeProof.Sequences.Session.Run \
    --host-symbol proof_sequences --llc "$llc" --output "$proof/hosted-sequences.o"
"$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/SequencesHosted.cpp" "$proof/hosted-sequences.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/hosted-sequences"
env -i "$proof/hosted-sequences"

"$compiler" --source "$assets/Dictionaries.pipe" --entry NativeProof.Dictionaries.App.Run \
    --runtime --llc "$llc" --output "$proof/dictionaries.o"
"$cxx" -pthread -no-pie -fsanitize=address,undefined "${runtime_objects[@]}" \
    "$proof/native_app_launcher.o" "$proof/dictionaries.o" -o "$proof/dictionaries"
test "$(env -i "$proof/dictionaries" 0 --require-suspension)" = 42

"$compiler" --source "$assets/Dictionaries.pipe" --entry NativeProof.Dictionaries.Session.Run \
    --host-symbol proof_dictionaries --llc "$llc" --output "$proof/hosted-dictionaries.o"
"$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/DictionariesHosted.cpp" "$proof/hosted-dictionaries.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/hosted-dictionaries"
env -i "$proof/hosted-dictionaries"

verify_hosted_fixture() {
    local fixture=$1
    local name=$2
    local symbol=$3
    shift 3
    "$compiler" --source "$assets/$fixture.pipe" --entry "NativeProof.$fixture.Session.Run" \
        --host-symbol "$symbol" --llc "$llc" --output "$proof/$name.o" "$@"
    "$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$runtime/include" "$assets/$fixture.cpp" "$proof/$name.o" \
        "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/$name"
    env -i "$proof/$name"
}

verify_hosted_fixture TypedHostResults typed-host-results proof_typed_host_results
verify_hosted_fixture HostedSynchronous hosted-synchronous proof_hosted_synchronous
verify_hosted_fixture ResultInterface result-interface proof_result_interface
verify_hosted_fixture HostedResultInterface hosted-result-interface proof_hosted_result_interface
verify_hosted_fixture HostedInheritance hosted-inheritance proof_hosted_inheritance
verify_hosted_fixture AsyncRecursion async-recursion proof_async_recursion
verify_hosted_fixture ContextualRecursion contextual-recursion proof_contextual_recursion
verify_hosted_fixture Inheritance inheritance proof_inheritance
verify_hosted_fixture ConstructorRollback constructor-rollback proof_constructor_rollback
verify_hosted_fixture SharedSemaphores shared-semaphores proof_shared_semaphores
verify_hosted_fixture BlockingSemaphores blocking-semaphores proof_blocking_semaphores
verify_hosted_fixture SharedAtomics shared-atomics proof_shared_atomics
verify_hosted_fixture LockScopes lock-scopes proof_lock_scopes
verify_hosted_fixture ConvertedForeach converted-foreach proof_converted_foreach
verify_hosted_fixture ParallelWorkers parallel-workers proof_parallel_workers
verify_hosted_fixture ConcurrentSequences concurrent-sequences proof_concurrent_sequences
verify_hosted_fixture ConcurrentDictionaries concurrent-dictionaries proof_concurrent_dictionaries
verify_hosted_fixture HostedSynchronization hosted-synchronization proof_hosted_synchronization
verify_hosted_fixture HostedGraphCycles hosted-graph-cycles proof_hosted_graph_cycles
verify_hosted_fixture HostedGraphSynchronization hosted-graph-synchronization proof_hosted_graph_synchronization
verify_hosted_fixture HostedInitialGraph hosted-initial-graph proof_hosted_initial_graph
verify_hosted_fixture HostedInterfaceField hosted-interface-field proof_hosted_interface_field
verify_hosted_fixture HostedGraphAsync hosted-graph-async proof_hosted_graph_async
verify_hosted_fixture HostedModelFacetGraph hosted-model-facet-graph proof_hosted_model_facet_graph
verify_hosted_fixture FloatingObservation floating-observation proof_floating_observation
verify_hosted_fixture HostedCallValues hosted-call-values proof_hosted_call_values \
    --host-method Inspect --host-method Clear --host-method Check
verify_hosted_fixture PersistentSets persistent-sets proof_persistent_sets
verify_hosted_fixture PersistentReportValues persistent-report-values proof_persistent_report_values
verify_hosted_fixture FixedArrayClaims fixed-array-claims proof_fixed_array_claims --host-snapshot
verify_hosted_fixture ArrayValues array-values proof_array_values
verify_hosted_fixture ArrayHostValues array-host-values proof_array_host_values

"$compiler" --source "$assets/HostedInstance.pipe" --entry NativeProof.HostedInstance.Session.Run \
    --host-symbol proof_hosted_session --llc "$llc" --output "$proof/hosted-instance.o"
"$compiler" --source "$assets/HostedReport.pipe" --entry NativeProof.HostedReport.Session.Run \
    --host-symbol proof_hosted_report --llc "$llc" --output "$proof/hosted-report.o"
"$compiler" --source "$assets/HostedClock.pipe" --entry NativeProof.HostedClock.Session.Run \
    --host-symbol proof_hosted_clock --llc "$llc" --output "$proof/hosted-clock.o"
"$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/HostedInstance.cpp" "$proof/hosted-instance.o" "$proof/hosted-report.o" "$proof/hosted-clock.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/hosted-instance"
env -i "$proof/hosted-instance"

"$compiler" --source "$assets/HostedReceivers.pipe" --entry NativeProof.HostedReceivers.Session.Run \
    --host-symbol proof_hosted_receivers --llc "$llc" --output "$proof/hosted-receivers.o"
"$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/HostedReceivers.cpp" "$proof/hosted-receivers.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" -o "$proof/hosted-receivers"
env -i "$proof/hosted-receivers"

"$compiler" --source "$assets/HostedCompositionGraph.pipe" --entry NativeProof.ScopedPorts.Root.Read \
    --host-symbol proof_root --host-object-references --host-multiple-tasks \
    --host-method Next --host-method ReadLater --host-method Release \
    --host-composition NativeProof.ScopedPorts.Startup --llc "$llc" --output "$proof/hosted-composition-scopes.o"
"$cxx" -std=c++20 -O1 -g1 -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$runtime/include" "$assets/HostedCompositionScopes.cpp" "$proof/hosted-composition-scopes.o" \
    "${runtime_objects[@]}" "$proof/native_host.o" "$proof/native_composition.o" -o "$proof/hosted-composition-scopes"
env -i ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$proof/hosted-composition-scopes"

for fixture in AliasedExports CompositionFactories CompositionCleanup PolymorphicFactory; do
    "$compiler" --source "$assets/$fixture.pipe" --entry "NativeProof.$fixture.Root.Run" \
        --host-symbol proof_root --host-object-references --host-multiple-tasks \
        --host-composition "NativeProof.$fixture.Startup" --llc "$llc" --output "$proof/$fixture.o"
    "$cxx" -std=c++20 -O1 -g -pthread -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$runtime/include" "$assets/$fixture.cpp" "$proof/$fixture.o" \
        "${runtime_objects[@]}" "$proof/native_host.o" "$proof/native_composition.o" -o "$proof/$fixture"
    env -i "$proof/$fixture"
done
