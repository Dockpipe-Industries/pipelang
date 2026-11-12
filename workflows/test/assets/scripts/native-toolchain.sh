#!/usr/bin/env sh
set -eu
platform_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$platform_directory/../../../build/assets/scripts/platform.sh"
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
    exec "$runner" job --output "$PIPELANG_NATIVE_COMPAT_PROOF/job" \
        --estate "$PIPELANG_NATIVE_COMPAT_PROOF" --timeout 7200 -- \
        /usr/bin/env PIPELANG_NATIVE_TOOLCHAIN_PROOF="$PIPELANG_NATIVE_TOOLCHAIN_PROOF" \
        PIPELANG_NATIVE_BUILD_DIRECTORY="$build" PIPELANG_NATIVE_COMPAT_PROOF="$PIPELANG_NATIVE_COMPAT_PROOF" \
        PIPELANG_NATIVE_COMPAT_SUITES="${PIPELANG_NATIVE_COMPAT_SUITES:-protocol fundamentals iteration async-composition failure-cleanup}" \
        PIPELANG_NATIVE_RUNTIME_SELECTION="${PIPELANG_NATIVE_RUNTIME_SELECTION:-all}" \
        PIPELANG_NATIVE_FIXTURE_SELECTION="${PIPELANG_NATIVE_FIXTURE_SELECTION-all}" \
        PIPELANG_NATIVE_TOOL_PATH="${PIPELANG_NATIVE_TOOL_PATH:-}" sh "$0" --inside
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
    case "$unit_name" in
        *-sanitizer-run)
            # LeakSanitizer refuses ptrace. These source-bound C++ fixtures do
            # not launch child executables; trace their unsanitized runs instead.
            ;;
        *) if [ "$PIPELANG_NATIVE_PLATFORM" = linux ]; then
            set -- /usr/bin/strace -f -e trace=execve -s 4096 -o "$prefix.exec" "$@"
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
        semantics|synchronous-host|initialization-budget|list-reads|fundamentals|uint32|uint32-integer-only|iteration|async-composition|failure-cleanup|composition|parallel_source|collections_source|mixed|concurrent_source|synchronization_source|objects_source|objects_composition|interfaces_source|synchronous_host_objects|initialization_budget_objects|list_read_objects|uint32_objects|foundation|foundation_objects|byte_buffers|byte_buffer_objects|result_interface|result_propagation_worker|result_propagation|typed_host_errors|byte_buffer_iteration|native-numeric|project|compiled-project|class-inheritance|extended_collections|parallel_runtime|concurrent_runtime|synchronization_runtime|lazy_task_scope|synchronous_host_scope|initialization_budget_runtime|object_runtime|object_transaction) ;;
        *) echo "Unknown native fixture selection: $selected_fixture" >&2; exit 1 ;;
    esac
done
fixture_selection="$*"
printf '%s\n' "$fixture_selection" > "$proof/fixture-selection.txt"
executed_fixtures=''
unit sdk-verify-before "$sdk/bin/pipelang-sdk-tool" verify --prefix "$sdk" --manifest "$manifest" --sha256 "$expected"
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
    case " $fixture_selection " in
        *" all "*|*" $name "*) ;;
        *) return ;;
    esac
    executed_fixtures="$executed_fixtures $name"
    mkdir "$proof/$name-runtime"
    unit "$name-build" "$PIPELANG_CXX" -std=c++20 -pthread -Wall -Wextra -Werror -O2 \
        -I"$sdk/include" -I"$generated" "$source" -o "$proof/$name-runtime/test"
    unit "$name-run" "$proof/$name-runtime/test"
    unit "$name-sanitizer-build" "$PIPELANG_CXX" -std=c++20 -pthread -Wall -Wextra -Werror -O1 -g \
        -fsanitize=address,undefined -fno-omit-frame-pointer -I"$sdk/include" -I"$generated" \
        "$source" -o "$proof/$name-runtime/test-sanitized"
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
            unit uint32-integer-fast-build "$PIPELANG_CXX" -std=c++20 -pthread -Wall -Wextra -Werror -O2 -ffast-math \
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
                -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
                -DSTRUCTURED_SDK_PREFIX="$sdk" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
                -DFIXED_ARRAY_SDK_PREFIX="$sdk" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
                -DFOR_LOOP_SDK_PREFIX="$sdk" -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
                -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
        unit compiled-project-sanitizer-configure cmake -S "$proof/compiled-project" \
            -B "$proof/compiled-project/sanitizer" -DCMAKE_PREFIX_PATH="$sdk" \
            -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release \
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
            -DCMAKE_CXX_COMPILER="$PIPELANG_CXX" -DCMAKE_BUILD_TYPE=Release -DPURE_HOST_SANITIZERS=ON
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
case "$PIPELANG_NATIVE_RUNTIME_SELECTION" in
    all)
        for fixture in extended_collections parallel_runtime concurrent_runtime synchronization_runtime lazy_task_scope synchronous_host_scope initialization_budget_runtime object_runtime object_transaction; do
            compile_fixture "$fixture" "tests/native-models/$fixture.cpp" "$sdk/include"
        done
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
