#!/usr/bin/env sh
set -eu
platform_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$platform_directory/../../../build/assets/scripts/platform.sh"
. "$platform_directory/../../../build/assets/scripts/cxx-resource-profile.sh"
runtime_linkage=${PIPELANG_NATIVE_RUNTIME_LINKAGE-header-only}
case "$runtime_linkage" in
    header-only) ;;
    linked)
        if [ "$PIPELANG_NATIVE_PLATFORM" != linux ]; then
            echo 'Linked native workflow runtime is supported only on Linux' >&2
            exit 1
        fi
        ;;
    *) echo 'PIPELANG_NATIVE_RUNTIME_LINKAGE must be header-only or linked' >&2; exit 1 ;;
esac
if [ -n "${PIPELANG_NATIVE_TOOL_PATH:-}" ]; then
    PATH=$PIPELANG_NATIVE_TOOL_PATH
    export PATH
fi
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Select the completed native-toolchain build proof}"
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
runner="$build/containedexec/pipelang-contained"
probe="$build/containedexec/pipelang-containment-probe"
test -x "$runner"
test -x "$probe"
if [ "${1:-}" != --inside ]; then
    if [ -z "${PIPELANG_NATIVE_CONTAINMENT_ADMISSION:-}" ] && [ -f "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/installed-containment/acceptance.json" ]; then
        PIPELANG_NATIVE_CONTAINMENT_ADMISSION="$PIPELANG_NATIVE_TOOLCHAIN_PROOF/installed-containment/acceptance.json"
        PIPELANG_NATIVE_CONTAINMENT_ADMISSION_SHA256=$(sha256sum "$PIPELANG_NATIVE_CONTAINMENT_ADMISSION" | cut -d' ' -f1)
    fi
    if [ -n "${PIPELANG_NATIVE_CONTAINMENT_ADMISSION:-}" ]; then
        : "${PIPELANG_NATIVE_CONTAINMENT_ADMISSION_SHA256:?Pin the prior native containment acceptance receipt}"
        "$probe" --admit "$runner" "$PIPELANG_NATIVE_CONTAINMENT_ADMISSION" "$PIPELANG_NATIVE_CONTAINMENT_ADMISSION_SHA256"
    else
        "$probe" --suite "$runner" "${PIPELANG_NATIVE_CONTAINMENT_PROOF:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/native-containment}"
    fi
    : "${PIPELANG_NATIVE_COMPAT_PROOF:?Select a fresh compatibility test proof}"
    mkdir "$PIPELANG_NATIVE_COMPAT_PROOF"
    run_native_job "$runner" "$PIPELANG_NATIVE_COMPAT_PROOF/job" \
        --estate "$PIPELANG_NATIVE_COMPAT_PROOF" --timeout 7200 -- \
        /usr/bin/env PIPELANG_NATIVE_TOOLCHAIN_PROOF="$PIPELANG_NATIVE_TOOLCHAIN_PROOF" \
        PIPELANG_NATIVE_BUILD_DIRECTORY="$build" PIPELANG_NATIVE_COMPAT_PROOF="$PIPELANG_NATIVE_COMPAT_PROOF" \
        PIPELANG_NATIVE_COMPAT_SUITES="${PIPELANG_NATIVE_COMPAT_SUITES:-protocol fundamentals iteration async-composition failure-cleanup}" \
        PIPELANG_NATIVE_RUNTIME_SELECTION="${PIPELANG_NATIVE_RUNTIME_SELECTION:-all}" \
        PIPELANG_NATIVE_RUNTIME_LINKAGE="$runtime_linkage" \
        PIPELANG_NATIVE_FIXTURE_SELECTION="${PIPELANG_NATIVE_FIXTURE_SELECTION-all}" \
        PIPELANG_CXX_RESOURCE_PROFILE="${PIPELANG_CXX_RESOURCE_PROFILE-system}" \
        PIPELANG_NATIVE_TOOL_PATH="${PIPELANG_NATIVE_TOOL_PATH:-}" sh "$0" --inside
    exit
fi
if [ "$PIPELANG_NATIVE_PLATFORM" = macos ]; then
    : "${PIPELANG_MACOS_COORDINATOR:?Native workflow requires a macOS coordinator}"
else
    : "${CONTAINED_JOB_SLICE:?Tests require verified aggregate containment}"
fi
proof=$PIPELANG_NATIVE_COMPAT_PROOF
sdk="$PIPELANG_NATIVE_TOOLCHAIN_PROOF/sdk"
manifest="$sdk/share/pipelang-model/sdk-manifest.json"
expected=$(sha256sum "$manifest" | cut -d' ' -f1)
mkdir "$proof/units"
unit() {
    unit_name=$1
    shift
    prefix="$proof/units/$unit_name"
    # Linux unit execution transports only its declared environment.
    # Forward selection explicitly for SDK-owned nested CMake consumers too.
    set -- /usr/bin/env "PIPELANG_CXX_RESOURCE_PROFILE=${PIPELANG_CXX_RESOURCE_PROFILE-system}" "$@"
    case "$unit_name" in
        *-sanitizer-run)
            # LeakSanitizer refuses ptrace. These source-bound C++ fixtures do
            # not launch child executables; trace their unsanitized runs instead.
            ;;
        *) if [ "$PIPELANG_NATIVE_PLATFORM" = linux ]; then
            set -- /bin/sh "$platform_directory/../../../build/assets/scripts/trace-executions.sh" "$prefix.exec" "$@"
           fi ;;
    esac
    if "$runner" unit --output "$prefix" --estate "$proof" --timeout 900 -- "$@" > "$prefix.launcher.log" 2>&1; then
        echo "$unit_name: passed"
        return
    fi
    tail -c 6000 "$prefix.output" >&2
    tail -c 3000 "$prefix.launcher.log" >&2
    exit 1
}
fixture_selection=${PIPELANG_NATIVE_FIXTURE_SELECTION-all}
set -f
set -- $fixture_selection
set +f
if [ "$#" -eq 0 ]; then
    echo 'Native fixture selection must not be empty' >&2
    exit 1
fi
for selected_fixture in "$@"; do
    case "$selected_fixture" in
        all)
            if [ "$#" -ne 1 ]; then
                echo 'Native fixture selection all must be used alone' >&2
                exit 1
            fi
            ;;
        semantics|synchronous-host|initialization-budget|list-reads|fundamentals|uint32|uint32-integer-only|iteration|async-composition|failure-cleanup|composition|parallel_source|collections_source|mixed|concurrent_source|synchronization_source|objects_source|objects_composition|interfaces_source|synchronous_host_objects|initialization_budget_objects|list_read_objects|uint32_objects|foundation|foundation_objects|byte_buffers|byte_buffer_objects|result_interface|result_propagation_worker|result_propagation|typed_host_errors|byte_buffer_iteration|generic_member_calls|static_methods_objects|static_methods_project|static_methods_compiled|var_inference_objects|var_inference_project|var_inference_compiled|bounded_calls_objects|bounded_calls_project|bounded_calls_compiled|bounded_calls_runtime|execution_segments_objects|execution_segments_project|execution_segments_compiled|execution_segments_di_project|execution_segments_di_compiled|execution_segments_runtime|member_bindings_objects|member_bindings_project|member_bindings_compiled|async_finally_source|async_finally_project|async_finally_compiled|async_finally_probe|async_finally_runtime|async_finally_storage_runtime|loop_flow_objects|loop_flow_project|loop_flow_compiled|testing_library|independent_tokens_source|independent_tokens_project|independent_tokens_compiled|independent_tokens_di_project|independent_tokens_di_compiled|empty_interfaces_objects|empty_interfaces_project|empty_interfaces_compiled|empty_interfaces_project_types|empty_interfaces_compiled_types|testing_async_library|json_library|native-numeric|project|compiled-project|class-inheritance|extended_collections|parallel_runtime|concurrent_runtime|synchronization_runtime|lazy_task_scope|synchronous_host_scope|initialization_budget_runtime|object_runtime|object_transaction) ;;
        *) echo "Unknown native fixture selection: $selected_fixture" >&2; exit 1 ;;
    esac
done
fixture_selection="$*"
printf '%s\n' "$fixture_selection" > "$proof/fixture-selection.txt"
executed_fixtures=''
unit sdk-verify-before "$sdk/bin/pipelang-sdk-tool" verify --prefix "$sdk" --manifest "$manifest" --sha256 "$expected"
select_cxx_resource_profile "$sdk" "$proof"
printf '%s\n' "$runtime_linkage" > "$proof/runtime-linkage.txt"
linked_runtime_normal=''
linked_runtime_sanitizer=''
if [ "$runtime_linkage" = linked ]; then
    for linked_mode in normal sanitizer; do
        linked_output="$proof/linked-runtime-$linked_mode"
        unit "linked-runtime-$linked_mode-prepare" "$PIPELANG_CMAKE" \
            "-DSDK_PREFIX=$sdk" "-DOUTPUT_ROOT=$linked_output" \
            "-DCXX_COMPILER=$PIPELANG_CXX" "-DRESOURCE_PROFILE=$PIPELANG_CXX_RESOURCE_PROFILE" \
            "-DRUNTIME_MODE=$linked_mode" \
            -P "$platform_directory/../linked-runtime/prepare.cmake"
        IFS= read -r linked_archive < "$linked_output/archive-path.txt"
        case "$linked_archive" in
            "$linked_output"/*) ;;
            *) echo 'Linked runtime archive escaped its fresh proof directory' >&2; exit 1 ;;
        esac
        if [ ! -f "$linked_archive" ] || [ -L "$linked_archive" ]; then
            echo 'Linked runtime archive must be a regular file' >&2
            exit 1
        fi
        case "$linked_mode" in
            normal) linked_runtime_normal=$linked_archive ;;
            sanitizer) linked_runtime_sanitizer=$linked_archive ;;
        esac
    done
fi
unit cxx-resource-profile-contracts "$PIPELANG_CMAKE" \
    "-DSDK_PREFIX=$sdk" "-DSOURCE_ROOT=$PWD" \
    "-DOUTPUT_ROOT=$proof/compile-resource-profile-contracts" \
    "-DCXX_COMPILER=$PIPELANG_CXX" '-DGENERATOR=Unix Makefiles' \
    -P "$PWD/tests/compile-resource-profile/Contracts.cmake"
oracle="$build/sdk-tools/pipelang-model-compat-tests"
for suite in $PIPELANG_NATIVE_COMPAT_SUITES; do
    case "$suite" in
        protocol|fundamentals|iteration|async-composition|failure-cleanup) ;;
        none) continue ;;
        *) echo 'Unknown native compatibility suite' >&2; exit 1 ;;
    esac
    unit "$suite-generate" "$oracle" --suite "$suite" --compiler "$sdk/bin/pipelang-modelc" --source-root "$PWD" --output "$proof/$suite"
done
compile_fixture() {
    name=$1
    source=$2
    generated=$3
    shift 3
    case " $fixture_selection " in
        *" all "*|*" $name "*) ;;
        *) return ;;
    esac
    executed_fixtures="$executed_fixtures $name"
    mkdir "$proof/$name-runtime"
    if [ "$runtime_linkage" = linked ]; then
        cxx_unit "$name-build" -std=c++20 -pthread -Wall -Wextra -Werror -O2 \
            -DPIPELANG_MODEL_LINKED_RUNTIME=1 -I"$sdk/include" -I"$generated" \
            "$source" "$@" "$linked_runtime_normal" -o "$proof/$name-runtime/test"
    else
        cxx_unit "$name-build" -std=c++20 -pthread -Wall -Wextra -Werror -O2 \
            -I"$sdk/include" -I"$generated" "$source" "$@" -o "$proof/$name-runtime/test"
    fi
    unit "$name-run" "$proof/$name-runtime/test"
    if [ "$runtime_linkage" = linked ]; then
        cxx_unit "$name-sanitizer-build" -std=c++20 -pthread -Wall -Wextra -Werror -O1 -g1 \
            -fsanitize=address,undefined -fno-omit-frame-pointer -DPIPELANG_MODEL_LINKED_RUNTIME=1 \
            -I"$sdk/include" -I"$generated" "$source" "$@" "$linked_runtime_sanitizer" \
            -o "$proof/$name-runtime/test-sanitized"
    else
        cxx_unit "$name-sanitizer-build" -std=c++20 -pthread -Wall -Wextra -Werror -O1 -g1 \
            -fsanitize=address,undefined -fno-omit-frame-pointer -I"$sdk/include" -I"$generated" \
            "$source" "$@" -o "$proof/$name-runtime/test-sanitized"
    fi
    unit "$name-sanitizer-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$name-runtime/test-sanitized"
}
for suite in $PIPELANG_NATIVE_COMPAT_SUITES; do
    case "$suite" in
        protocol)
            compile_fixture semantics tests/native-models/semantics.cpp "$proof/protocol"
            compile_fixture synchronous-host tests/native-models/synchronous_host.cpp "$proof/protocol"
            compile_fixture initialization-budget tests/native-models/initialization_budget.cpp "$proof/protocol"
            compile_fixture list-reads tests/native-models/list_reads.cpp "$proof/protocol"
            ;;
        fundamentals)
            compile_fixture fundamentals tests/native-models/fundamentals.cpp "$proof/fundamentals"
            compile_fixture uint32 tests/native-models/uint32.cpp "$proof/fundamentals"
            compile_fixture uint32-integer-only tests/native-models/uint32_integer_only.cpp "$proof/fundamentals"
            cxx_unit uint32-integer-fast-build -std=c++20 -pthread -Wall -Wextra -Werror -O2 -ffast-math \
                -I"$sdk/include" -I"$proof/fundamentals" tests/native-models/uint32_integer_only.cpp -o "$proof/uint32-integer-fast"
            unit uint32-integer-fast-run "$proof/uint32-integer-fast"
            ;;

        iteration) compile_fixture iteration tests/native-models/iteration-tests.cpp "$proof/iteration" ;;
        async-composition) compile_fixture async-composition tests/native-models/async-composition.cpp "$proof/async-composition" ;;
        failure-cleanup) compile_fixture failure-cleanup tests/native-models/failure-cleanup.cpp "$proof/failure-cleanup" ;;
    esac
done
case " $PIPELANG_NATIVE_COMPAT_SUITES " in
    *" fundamentals "*) unit numeric-guards "$build/sdk-tools/pipelang-extended-tests" --numeric-guard "$sdk" "$proof/fundamentals/NumericDefaults.hpp" ;;
esac
unit extended-generate "$build/sdk-tools/pipelang-extended-tests" "$PWD" "$sdk" "$proof/extended"
for fixture in composition parallel_source collections_source mixed concurrent_source synchronization_source objects_source objects_composition interfaces_source synchronous_host_objects initialization_budget_objects list_read_objects uint32_objects foundation foundation_objects byte_buffers byte_buffer_objects result_interface result_propagation_worker result_propagation typed_host_errors byte_buffer_iteration; do
    compile_fixture "$fixture" "tests/native-models/$fixture.cpp" "$proof/extended"
done
unit native-numeric-generate "$build/sdk-tools/pipelang-sdk-tool-tests" --native-numeric-fixture "$sdk" "$proof/native-numeric"
compile_fixture native-numeric "$proof/native-numeric/main.cpp" "$proof/native-numeric/generated"
case " $fixture_selection " in
    *" all "*|*" native-numeric "*)
        active_project="$proof/native-numeric/active"
        for active_mode in normal sanitizer; do
            active_sanitizers=OFF
            if [ "$active_mode" = sanitizer ]; then
                active_sanitizers=ON
            fi
            active_build="$active_project/$active_mode"
            unit "active-numeric-$active_mode-configure" "$PIPELANG_CMAKE" -S "$active_project" \
                -B "$active_build" -DCMAKE_PREFIX_PATH="$sdk" \
                -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                -DACTIVE_SANITIZERS="$active_sanitizers"
            unit "active-numeric-$active_mode-build" "$PIPELANG_CMAKE" --build "$active_build" --parallel 1
            if [ "$active_mode" = sanitizer ]; then
                unit active-numeric-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$active_build/active_numeric"
            else
                unit active-numeric-normal-run "$active_build/active_numeric"
            fi
        done
        structured_project="$proof/native-numeric/structured-values"
        for structured_mode in normal sanitizer; do
            structured_sanitizers=OFF
            if [ "$structured_mode" = sanitizer ]; then
                structured_sanitizers=ON
            fi
            structured_build="$structured_project/$structured_mode"
            unit "structured-values-$structured_mode-configure" "$PIPELANG_CMAKE" -S "$structured_project" -B "$structured_build" \
                -DSTRUCTURED_SDK_PREFIX="$sdk" -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                -DSTRUCTURED_SANITIZERS="$structured_sanitizers"
            unit "structured-values-$structured_mode-build" "$PIPELANG_CMAKE" --build "$structured_build" --parallel 1
            for structured_executable in structured_values structured_headers; do
                if [ "$structured_mode" = sanitizer ]; then
                    unit "structured-values-$structured_executable-sanitizer-run" /usr/bin/env \
                        ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                        "$structured_build/$structured_executable"
                else
                    unit "structured-values-$structured_executable-normal-run" "$structured_build/$structured_executable"
                fi
            done
        done
        fixed_array_project="$proof/native-numeric/fixed-arrays"
        for fixed_array_mode in normal sanitizer; do
            fixed_array_sanitizers=OFF
            if [ "$fixed_array_mode" = sanitizer ]; then
                fixed_array_sanitizers=ON
            fi
            fixed_array_build="$fixed_array_project/$fixed_array_mode"
            unit "fixed-arrays-$fixed_array_mode-configure" "$PIPELANG_CMAKE" -S "$fixed_array_project" -B "$fixed_array_build" \
                -DFIXED_ARRAY_SDK_PREFIX="$sdk" -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                -DFIXED_ARRAY_SANITIZERS="$fixed_array_sanitizers"
            unit "fixed-arrays-$fixed_array_mode-build" "$PIPELANG_CMAKE" --build "$fixed_array_build" --parallel 1
            for fixed_array_executable in fixed_arrays fixed_array_headers; do
                if [ "$fixed_array_mode" = sanitizer ]; then
                    unit "fixed-arrays-$fixed_array_executable-sanitizer-run" /usr/bin/env \
                        ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                        "$fixed_array_build/$fixed_array_executable"
                else
                    unit "fixed-arrays-$fixed_array_executable-normal-run" "$fixed_array_build/$fixed_array_executable"
                fi
            done
        done
        for_loop_project="$proof/native-numeric/for-loops"
        for for_loop_mode in normal sanitizer; do
            for_loop_sanitizers=OFF
            if [ "$for_loop_mode" = sanitizer ]; then
                for_loop_sanitizers=ON
            fi
            for_loop_build="$for_loop_project/$for_loop_mode"
            unit "for-loops-$for_loop_mode-configure" "$PIPELANG_CMAKE" -S "$for_loop_project" -B "$for_loop_build" \
                -DFOR_LOOP_SDK_PREFIX="$sdk" -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                -DFOR_LOOP_SANITIZERS="$for_loop_sanitizers"
            unit "for-loops-$for_loop_mode-build" "$PIPELANG_CMAKE" --build "$for_loop_build" --parallel 1
            for for_loop_executable in for_loops for_loop_headers; do
                if [ "$for_loop_mode" = sanitizer ]; then
                    unit "for-loops-$for_loop_executable-sanitizer-run" /usr/bin/env \
                        ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                        "$for_loop_build/$for_loop_executable"
                else
                    unit "for-loops-$for_loop_executable-normal-run" "$for_loop_build/$for_loop_executable"
                fi
            done
        done
        observation_project="$proof/native-numeric/observation"
        for observation_mode in normal sanitizer; do
            observation_sanitizers=OFF
            if [ "$observation_mode" = sanitizer ]; then
                observation_sanitizers=ON
            fi
            observation_build="$observation_project/$observation_mode"
            unit "ordinary-observation-$observation_mode-configure" "$PIPELANG_CMAKE" -S "$observation_project" \
                -B "$observation_build" -DCMAKE_PREFIX_PATH="$sdk" \
                -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                -DOBSERVATION_SANITIZERS="$observation_sanitizers"
            unit "ordinary-observation-$observation_mode-build" "$PIPELANG_CMAKE" --build "$observation_build" --parallel 1
            if [ "$observation_mode" = sanitizer ]; then
                unit ordinary-observation-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$observation_build/ordinary_observation"
            else
                unit ordinary-observation-normal-run "$observation_build/ordinary_observation"
            fi
        done
        ;;
esac
unit project-generate "$build/sdk-tools/pipelang-sdk-tool-tests" --project-fixture "$sdk" "$proof/project"
compile_fixture project "$proof/project/main.cpp" "$proof/project/generated"
case " $fixture_selection " in
    *" all "*|*" compiled-project "*)
        executed_fixtures="$executed_fixtures compiled-project"
        unit compiled-project-contracts "$build/sdk-tools/pipelang-sdk-tool-tests" \
            --compiled-project-contracts "$sdk" "$proof/compiled-project"
        for owner_mode in normal sanitizer; do
            owner_fixture="$proof/compiled-owner-$owner_mode"
            unit "compiled-owner-$owner_mode-prepare" "$build/sdk-tools/pipelang-sdk-tool-tests" \
                --compiled-owner-prepare "$sdk" "$owner_fixture" "$owner_mode"
            if [ "$owner_mode" = sanitizer ]; then
                unit compiled-owner-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                    "$owner_fixture/consumer-build/owner_consumer"
            else
                unit compiled-owner-normal-run "$owner_fixture/consumer-build/owner_consumer"
            fi
        done
        if [ "$PIPELANG_NATIVE_PLATFORM" = linux ]; then
            for linked_owner_mode in normal sanitizer; do
                linked_owner_fixture="$proof/linked-runtime-owner-$linked_owner_mode"
                unit "linked-runtime-owner-$linked_owner_mode-prepare" "$build/sdk-tools/pipelang-sdk-tool-tests" \
                    --linked-runtime-prepare "$sdk" "$linked_owner_fixture" "$linked_owner_mode"
                if [ "$linked_owner_mode" = sanitizer ]; then
                    unit linked-runtime-owner-sanitizer-run /usr/bin/env \
                        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
                        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                        "$linked_owner_fixture/consumer-build/linked_owner_consumer"
                else
                    unit linked-runtime-owner-normal-run \
                        "$linked_owner_fixture/consumer-build/linked_owner_consumer"
                fi
            done
        fi
        unit compiled-project-sanitizer-configure cmake -S "$proof/compiled-project" \
            -B "$proof/compiled-project/sanitizer" -DCMAKE_PREFIX_PATH="$sdk" \
            -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
            -DPIPELANG_COMPILED_FIXTURE_SANITIZERS=ON
        unit compiled-project-sanitizer-policy "$build/sdk-tools/pipelang-sdk-tool-tests" \
            --compiled-project-policy "$proof/compiled-project/sanitizer" sanitizer
        unit compiled-project-sanitizer-build cmake --build "$proof/compiled-project/sanitizer" --parallel 1
        for executable in consumer types_consumer compiled_peer; do
            unit "compiled-project-$executable-sanitizer-run" /usr/bin/env \
                ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                "$proof/compiled-project/sanitizer/$executable"
        done
        unit pure-host-project-sanitizer-configure cmake -S "$proof/compiled-project/pure-host-values" \
            -B "$proof/compiled-project/pure-host-values/sanitizer" -DCMAKE_PREFIX_PATH="$sdk" \
            -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release -DPURE_HOST_SANITIZERS=ON
        unit pure-host-project-sanitizer-build cmake --build "$proof/compiled-project/pure-host-values/sanitizer" --parallel 1
        for executable in pure_header_only_test pure_compiled_test; do
            unit "pure-host-project-$executable-sanitizer-run" /usr/bin/env \
                ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                "$proof/compiled-project/pure-host-values/sanitizer/$executable"
        done
        ;;
esac
class_oracle="$build/model-compiler/pipelang-class-inheritance-compiler-tests"
unit class-inheritance-generate "$class_oracle" "$PWD" "$proof/class-inheritance"
for fixture in ClassInheritance ClassModules; do
    unit "$fixture-transport" "$class_oracle" --transport "$sdk/bin/pipelang-modelc" "$proof/class-inheritance/$fixture.request.json" "$proof/class-inheritance/$fixture.actual.json"
    unit "$fixture-verify" "$class_oracle" --verify "$proof/class-inheritance/$fixture.expected.json" "$proof/class-inheritance/$fixture.actual.json"
done
compile_fixture class-inheritance tests/native-models/class_inheritance_runtime.cpp "$proof/class-inheritance"
unit compiler-intelligence "$build/model-compiler/pipelang-compiler-intelligence-tests"
unit source-formatting "$build/model-compiler/pipelang-source-formatting-tests"

unit generic-member-generate "$build/model-compiler/pipelang-generic-member-compiler-tests" "$PWD" "$proof/generic-member"
compile_fixture generic_member_calls tests/native-models/generic_member_calls_runtime.cpp "$proof/generic-member"
compile_fixture static_methods_objects tests/native-models/static_methods_source.cpp "$proof/generic-member/static-objects"
compile_fixture static_methods_project tests/native-models/static_methods_source.cpp "$proof/generic-member/static-project" \
    -DPIPELANG_STATIC_PROJECT=1
set -- "$proof/generic-member/static-compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Static methods compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture static_methods_compiled tests/native-models/static_methods_source.cpp "$proof/generic-member/static-compiled" \
    -DPIPELANG_STATIC_PROJECT=1 "$@"
unit var-inference-generate "$build/model-compiler/pipelang-var-inference-compiler-tests" "$PWD" "$proof/var-inference"
compile_fixture var_inference_objects tests/native-models/var_inference_source.cpp "$proof/var-inference/objects"
compile_fixture var_inference_project tests/native-models/var_inference_source.cpp "$proof/var-inference/project" \
    -DPIPELANG_VAR_PROJECT=1
set -- "$proof/var-inference/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Var inference compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture var_inference_compiled tests/native-models/var_inference_source.cpp "$proof/var-inference/compiled" \
    -DPIPELANG_VAR_PROJECT=1 "$@"
unit bounded-calls-generate "$build/model-compiler/pipelang-bounded-calls-compiler-tests" "$PWD" "$proof/bounded-calls"
compile_fixture bounded_calls_objects tests/native-models/bounded_calls_source.cpp "$proof/bounded-calls/objects"
compile_fixture bounded_calls_project tests/native-models/bounded_calls_source.cpp "$proof/bounded-calls/project" \
    -DPIPELANG_BOUNDED_PROJECT=1
# The oracle requires a fresh output directory and emits every declared implementation unit.
# Keep each path a separate argument, including when the proof directory contains spaces.
set -- "$proof/bounded-calls/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Bounded calls compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture bounded_calls_compiled tests/native-models/bounded_calls_source.cpp "$proof/bounded-calls/compiled" \
    -DPIPELANG_BOUNDED_PROJECT=1 "$@"
unit member-bindings-generate "$build/model-compiler/pipelang-member-bindings-compiler-tests" "$PWD" "$proof/member-bindings"
compile_fixture member_bindings_objects tests/native-models/member_bindings_source.cpp "$proof/member-bindings/objects"
compile_fixture member_bindings_project tests/native-models/member_bindings_source.cpp "$proof/member-bindings/project" \
    -DPIPELANG_BINDING_PROJECT=1
# Link every implementation emitted into the fresh compiler-oracle output.
set -- "$proof/member-bindings/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Member bindings compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture member_bindings_compiled tests/native-models/member_bindings_source.cpp "$proof/member-bindings/compiled" \
    -DPIPELANG_BINDING_PROJECT=1 "$@"
unit async-finally-generate "$build/model-compiler/pipelang-async-finally-compiler-tests" "$PWD" "$proof/async-finally"
compile_fixture async_finally_source tests/native-models/async_finally_source.cpp "$proof/async-finally/objects"
compile_fixture async_finally_project tests/native-models/async_finally_source.cpp "$proof/async-finally/project" \
    -DPIPELANG_ASYNC_FINALLY_PROJECT=1
set -- "$proof/async-finally/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Async finally compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture async_finally_compiled tests/native-models/async_finally_source.cpp "$proof/async-finally/compiled" \
    -DPIPELANG_ASYNC_FINALLY_PROJECT=1 "$@"
compile_fixture async_finally_probe tests/native-models/async_finally_probe.cpp "$proof/async-finally/probe"
unit execution-segments-generate "$build/model-compiler/pipelang-execution-segments-compiler-tests" "$PWD" "$proof/execution-segments"
compile_fixture execution_segments_objects tests/native-models/execution_segments_source.cpp "$proof/execution-segments/objects"
compile_fixture execution_segments_project tests/native-models/execution_segments_source.cpp "$proof/execution-segments/project" \
    -DPIPELANG_SEGMENTS_PROJECT=1
set -- "$proof/execution-segments/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Execution segments compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture execution_segments_compiled tests/native-models/execution_segments_source.cpp "$proof/execution-segments/compiled" \
    -DPIPELANG_SEGMENTS_PROJECT=1 "$@"
compile_fixture execution_segments_di_project tests/native-models/execution_segments_di.cpp "$proof/execution-segments/di_project" \
    -DPIPELANG_SEGMENTS_PROJECT=1
set -- "$proof/execution-segments/di_compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Execution segments DI compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture execution_segments_di_compiled tests/native-models/execution_segments_di.cpp "$proof/execution-segments/di_compiled" \
    -DPIPELANG_SEGMENTS_PROJECT=1 "$@"
unit independent-tokens-generate "$build/model-compiler/pipelang-independent-tokens-compiler-tests" "$PWD" "$proof/independent-tokens"
compile_fixture independent_tokens_source tests/native-models/independent_tokens_source.cpp "$proof/independent-tokens/objects"
compile_fixture independent_tokens_project tests/native-models/independent_tokens_source.cpp "$proof/independent-tokens/project" \
    -DPIPELANG_TOKENS_PROJECT=1
set -- "$proof/independent-tokens/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Independent tokens compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture independent_tokens_compiled tests/native-models/independent_tokens_source.cpp "$proof/independent-tokens/compiled" \
    -DPIPELANG_TOKENS_PROJECT=1 "$@"
compile_fixture independent_tokens_di_project tests/native-models/independent_tokens_di.cpp "$proof/independent-tokens/di_project" \
    -DPIPELANG_TOKENS_PROJECT=1
set -- "$proof/independent-tokens/di_compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Independent tokens DI compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture independent_tokens_di_compiled tests/native-models/independent_tokens_di.cpp "$proof/independent-tokens/di_compiled" \
    -DPIPELANG_TOKENS_PROJECT=1 "$@"
empty_interfaces="$proof/empty-source-interfaces"
empty_source=tests/native-models/empty-source-interfaces/empty_source_interface_source.cpp
empty_types=tests/native-models/empty-source-interfaces/empty_source_interface_types.cpp
unit empty-source-interface-generate "$build/model-compiler/pipelang-empty-source-interface-compiler-tests" "$PWD" "$empty_interfaces"
compile_fixture empty_interfaces_objects "$empty_source" "$empty_interfaces/objects"
compile_fixture empty_interfaces_project "$empty_source" "$empty_interfaces/project"
compile_fixture empty_interfaces_project_types "$empty_types" "$empty_interfaces/project"
set -- "$empty_interfaces/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Empty source interface compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture empty_interfaces_compiled "$empty_source" "$empty_interfaces/compiled" "$@"
compile_fixture empty_interfaces_compiled_types "$empty_types" "$empty_interfaces/compiled"
unit async-finally-scaling-compiler "$build/model-compiler/pipelang-async-finally-scaling-compiler-tests" "$PWD" "$proof/async-finally-scaling"
unit async-finally-storage "$build/model-compiler/pipelang-async-finally-storage-tests"
unit loop-flow-generate "$build/model-compiler/pipelang-loop-flow-compiler-tests" "$PWD" "$proof/loop-flow"
compile_fixture loop_flow_objects tests/native-models/loop_flow_source.cpp "$proof/loop-flow/objects"
compile_fixture loop_flow_project tests/native-models/loop_flow_source.cpp "$proof/loop-flow/project" \
    -DPIPELANG_LOOP_FLOW_PROJECT=1
set -- "$proof/loop-flow/compiled"/Implementation_*.cpp
if [ ! -f "$1" ]; then
    echo 'Loop flow compiled fixture has no implementation units' >&2
    exit 1
fi
compile_fixture loop_flow_compiled tests/native-models/loop_flow_source.cpp "$proof/loop-flow/compiled" \
    -DPIPELANG_LOOP_FLOW_PROJECT=1 "$@"
case " $fixture_selection " in
    *" all "*|*" testing_library "*)
        executed_fixtures="$executed_fixtures testing_library"
        testing_package="$sdk/lib/cmake/PipeLangModel"
        test -f "$testing_package/PipeLangModelConfig.cmake"
        for testing_mode in normal sanitizer; do
            testing_sanitizers=OFF
            if [ "$testing_mode" = sanitizer ]; then
                testing_sanitizers=ON
            fi
            testing_build="$proof/testing-library/$testing_mode"
            unit "testing-library-$testing_mode-configure" "$PIPELANG_CMAKE" \
                -S "$PWD/tests/testing" -B "$testing_build" \
                -DCMAKE_PREFIX_PATH="$sdk" -DPipeLangModel_DIR="$testing_package" \
                -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' \
                -DPIPELANG_TESTING_SANITIZERS="$testing_sanitizers"
            unit "testing-library-$testing_mode-build" "$PIPELANG_CMAKE" --build "$testing_build" \
                --target testing_contracts testing_probe_contracts --parallel 1
            if [ "$testing_mode" = sanitizer ]; then
                unit testing-library-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$testing_build/testing_contracts"
                unit testing-probe-library-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$testing_build/testing_probe_contracts"
            else
                unit testing-library-normal-run "$testing_build/testing_contracts"
                unit testing-probe-library-normal-run "$testing_build/testing_probe_contracts"
            fi
        done
        ;;
esac
case " $fixture_selection " in
    *" all "*|*" testing_async_library "*)
        executed_fixtures="$executed_fixtures testing_async_library"
        testing_package="$sdk/lib/cmake/PipeLangModel"
        test -f "$testing_package/PipeLangModelConfig.cmake"
        for testing_mode in normal sanitizer; do
            testing_sanitizers=OFF
            if [ "$testing_mode" = sanitizer ]; then
                testing_sanitizers=ON
            fi
            testing_build="$proof/testing-async-library/$testing_mode"
            unit "testing-async-library-$testing_mode-configure" "$PIPELANG_CMAKE" \
                -S "$PWD/tests/testing-async" -B "$testing_build" \
                -DCMAKE_PREFIX_PATH="$sdk" -DPipeLangModel_DIR="$testing_package" \
                -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
                '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' \
                -DPIPELANG_TESTING_SANITIZERS="$testing_sanitizers"
            unit "testing-async-library-$testing_mode-build" "$PIPELANG_CMAKE" --build "$testing_build" \
                --target testing_async_contracts --parallel 1
            if [ "$testing_mode" = sanitizer ]; then
                unit testing-async-library-sanitizer-run /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$testing_build/testing_async_contracts"
            else
                unit testing-async-library-normal-run "$testing_build/testing_async_contracts"
            fi
        done
        ;;
esac
case " $fixture_selection " in
    *" all "*|*" json_library "*)
        executed_fixtures="$executed_fixtures json_library"
        for json_mode in normal sanitizer; do
            json_sanitizers=OFF
            if [ "$json_mode" = sanitizer ]; then
                json_sanitizers=ON
            fi

            json_build="$proof/json-library/$json_mode"
            unit "json-library-$json_mode-configure" "$PIPELANG_CMAKE" \
                -S "$PWD/tests/json" -B "$json_build" \
                -DCMAKE_PREFIX_PATH="$sdk" -DPipeLangModel_DIR="$sdk/lib/cmake/PipeLangModel" \
                -DPipeLangModel_CXX_RESOURCE_PROFILE="$PIPELANG_CXX_RESOURCE_PROFILE" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" \
                -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' \
                -DPIPELANG_JSON_SANITIZERS="$json_sanitizers"
            unit "json-library-$json_mode-build" "$PIPELANG_CMAKE" --build "$json_build" --parallel 1 \
                --target json_contracts json_native_contracts json_maximum_input_contracts \
                    json_string_index_contracts json_string_index_source_contracts
            for json_test in json_contracts json_native_contracts json_maximum_input_contracts \
                json_string_index_contracts json_string_index_source_contracts; do
                unit "json-library-$json_test-$json_mode-run" /usr/bin/env \
                    ASAN_OPTIONS=detect_leaks=$([ "$PIPELANG_NATIVE_PLATFORM" = linux ] && echo 1 || echo 0):halt_on_error=1 \
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$json_build/$json_test"
            done
        done
        ;;
esac
case "$PIPELANG_NATIVE_RUNTIME_SELECTION" in
    all)
        for fixture in extended_collections parallel_runtime concurrent_runtime synchronization_runtime lazy_task_scope synchronous_host_scope initialization_budget_runtime execution_segments_runtime object_runtime object_transaction bounded_calls_runtime async_finally_runtime; do
            compile_fixture "$fixture" "tests/native-models/$fixture.cpp" "$sdk/include"
        done
        compile_fixture async_finally_storage_runtime tests/native-models/AsyncFinallyStorageTests.cpp "$sdk/include"
        ;;
    none) ;;
    *) echo 'Unknown native runtime selection' >&2; exit 1 ;;
esac
if [ "$fixture_selection" != all ]; then
    for selected_fixture in $fixture_selection; do
        case " $executed_fixtures " in
            *" $selected_fixture "*) ;;
            *) echo "Selected fixture unavailable in selected suites: $selected_fixture" >&2; exit 1 ;;
        esac
    done
fi
unit sdk-verify-after "$sdk/bin/pipelang-sdk-tool" verify --prefix "$sdk" --manifest "$manifest" --sha256 "$expected"
for trace in "$proof"/units/*.exec; do
    test -f "$trace" || continue
    name=$(basename "$trace" .exec)
    "$runner" unit --output "$proof/units/$name-exec-audit" --estate "$proof" --timeout 60 -- \
        "$sdk/bin/pipelang-sdk-tool" audit-exec --trace "$trace" --output "$trace.audit.json" \
        > "$proof/units/$name-exec-audit.launcher.log" 2>&1
done
