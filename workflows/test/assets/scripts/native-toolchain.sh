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
        protocol) compile_fixture semantics tests/native-models/semantics.cpp "$proof/protocol" ;;
        fundamentals) compile_fixture fundamentals tests/native-models/fundamentals.cpp "$proof/fundamentals" ;;
        iteration) compile_fixture iteration tests/native-models/iteration-tests.cpp "$proof/iteration" ;;
        async-composition) compile_fixture async-composition tests/native-models/async-composition.cpp "$proof/async-composition" ;;
        failure-cleanup) compile_fixture failure-cleanup tests/native-models/failure-cleanup.cpp "$proof/failure-cleanup" ;;
    esac
done
case " $PIPELANG_NATIVE_COMPAT_SUITES " in
    *" fundamentals "*) unit numeric-guards "$build/sdk-tools/pipelang-extended-tests" --numeric-guard "$sdk" "$proof/fundamentals/NumericDefaults.hpp" ;;
esac
unit extended-generate "$build/sdk-tools/pipelang-extended-tests" "$PWD" "$sdk" "$proof/extended"
for fixture in composition parallel_source collections_source mixed concurrent_source synchronization_source objects_source objects_composition interfaces_source; do
    compile_fixture "$fixture" "tests/native-models/$fixture.cpp" "$proof/extended"
done
class_oracle="$build/model-compiler/pipelang-class-inheritance-compiler-tests"
unit class-inheritance-generate "$class_oracle" "$PWD" "$proof/class-inheritance"
for fixture in ClassInheritance ClassModules; do
    unit "$fixture-transport" "$class_oracle" --transport "$sdk/bin/pipelang-modelc" "$proof/class-inheritance/$fixture.request.json" "$proof/class-inheritance/$fixture.actual.json"
    unit "$fixture-verify" "$class_oracle" --verify "$proof/class-inheritance/$fixture.expected.json" "$proof/class-inheritance/$fixture.actual.json"
done
compile_fixture class-inheritance tests/native-models/class_inheritance_runtime.cpp "$proof/class-inheritance"
case "$PIPELANG_NATIVE_RUNTIME_SELECTION" in
    all)
        for fixture in extended_collections parallel_runtime concurrent_runtime synchronization_runtime object_runtime object_transaction; do
            compile_fixture "$fixture" "tests/native-models/$fixture.cpp" "$sdk/include"
        done
        ;;
    none) ;;
    *) echo 'Unknown native runtime selection' >&2; exit 1 ;;
esac
unit sdk-verify-after "$sdk/bin/pipelang-sdk-tool" verify --prefix "$sdk" --manifest "$manifest" --sha256 "$expected"
for trace in "$proof"/units/*.exec; do
    test -f "$trace" || continue
    name=$(basename "$trace" .exec)
    "$runner" unit --output "$proof/units/$name-exec-audit" --estate "$proof" --timeout 60 -- \
        "$sdk/bin/pipelang-sdk-tool" audit-exec --trace "$trace" --output "$trace.audit.json" \
        > "$proof/units/$name-exec-audit.launcher.log" 2>&1
done
