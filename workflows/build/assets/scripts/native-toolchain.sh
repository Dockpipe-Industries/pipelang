#!/usr/bin/env sh
set -eu
if [ -n "${PIPELANG_NATIVE_TOOL_PATH:-}" ]; then
    PATH=$PIPELANG_NATIVE_TOOL_PATH
    export PATH
fi
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Select an absolute fresh native-toolchain proof directory}"
case "$PIPELANG_NATIVE_TOOLCHAIN_PROOF" in
    /*) ;;
    *) echo 'Native proof must be absolute' >&2; exit 1 ;;
esac
case "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/" in
    "$PWD/"*) echo 'Native proof must be outside sources' >&2; exit 1 ;;
esac
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
runner=${PIPELANG_NATIVE_RUNNER:-}
if [ -z "$runner" ]; then
    script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    if [ -n "${PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE:-}" ]; then
        . "$script_directory/bootstrap-native.sh"
    else
        . "$script_directory/bootstrap-source.sh"
    fi
fi
: "${PIPELANG_NATIVE_RUNNER_SHA256:?Pin the independently qualified native runner SHA256}"
actual_hash=$(sha256sum "$runner" | cut -d' ' -f1)
test "$actual_hash" = "$PIPELANG_NATIVE_RUNNER_SHA256"
if [ "${1:-}" != --inside ]; then
    mkdir "$PIPELANG_NATIVE_TOOLCHAIN_PROOF"
    exec "$runner" job --output "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/job" \
            --estate "$PIPELANG_NATIVE_TOOLCHAIN_PROOF" --timeout 3600 -- \
            /usr/bin/env PIPELANG_NATIVE_TOOLCHAIN_PROOF="$PIPELANG_NATIVE_TOOLCHAIN_PROOF" \
            PIPELANG_NATIVE_BUILD_DIRECTORY="$build" PIPELANG_NATIVE_RUNNER="$runner" \
            PIPELANG_NATIVE_TOOL_PATH="${PIPELANG_NATIVE_TOOL_PATH:-}" PIPELANG_NATIVE_TRACE_EXEC="${PIPELANG_NATIVE_TRACE_EXEC:-0}" \
            PIPELANG_NATIVE_RUNNER_SHA256="$PIPELANG_NATIVE_RUNNER_SHA256" sh "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Build requires verified aggregate containment}"
proof=$PIPELANG_NATIVE_TOOLCHAIN_PROOF
DOCKPIPE_EVENT_LOG="$proof/operation-events.jsonl"
export DOCKPIPE_EVENT_LOG
mkdir "$proof/units"
unit() {
    name=$1
    shift
    prefix="$proof/units/$name"
    if [ "${PIPELANG_NATIVE_TRACE_EXEC:-0}" = 1 ]; then
        set -- /usr/bin/strace -f -e trace=execve -s 4096 -o "$prefix.exec" "$@"
    fi
    if "$runner" unit --output "$prefix" --estate "$proof" --timeout 900 -- "$@" > "$prefix.launcher.log" 2>&1; then
        echo "$name: passed"
        return
    fi
    tail -c 6000 "$prefix.output" >&2
    tail -c 3000 "$prefix.launcher.log" >&2
    exit 1
}
unit configure /usr/bin/cmake -S sdk/pipeview/model -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPIPELANG_BUILD_SDK_TOOL_TESTS=ON -DPIPELANG_BUILD_CLASS_INHERITANCE_TESTS=ON
unit build /usr/bin/cmake --build "$build" --parallel 1
unit install /usr/bin/cmake --install "$build" --prefix "$proof/sdk"
if [ -f "$PIPELANG_NATIVE_TOOLCHAIN_PROOF-bootstrap/build.exec" ]; then
    unit bootstrap-exec-audit "$proof/sdk/bin/pipelang-sdk-tool" audit-exec --trace "$PIPELANG_NATIVE_TOOLCHAIN_PROOF-bootstrap/build.exec" --output "$proof/bootstrap.exec.audit.json"
fi
unit inventory "$proof/sdk/bin/pipelang-sdk-tool" inventory --source-root "$PWD" --output "$proof/inventory.json"
unit sdk-tests "$build/sdk-tools/pipelang-sdk-tool-tests" "$PWD" "$proof/sdk" "$proof/sdk-tests"
unit package-tests "$build/sdk-tools/pipelang-package-tests" "$proof/package-tests"
unit legacy-compiler-tests "$build/legacy-compiler/pipelang-legacy-compiler-tests" "$PWD" "$proof/legacy-compiler-tests" "$proof/sdk/bin/pipelang-native"
unit materialize-tests "$build/legacy-compiler/pipelang-materialize-tests" "$proof/materialize-tests" "$proof/sdk/bin/pipelang-native"
unit diagnostic-tests "$build/legacy-compiler/pipelang-diagnostic-tests" "$proof/diagnostic-tests" "$proof/sdk/bin/pipelang-native"
unit operation-tests "$build/legacy-compiler/pipelang-operation-tests" "$proof/operation-tests" "$proof/sdk/bin/pipelang-native"
unit companion-tests "$build/sdk-tools/pipelang-companion-tests" "$PWD" "$proof/sdk" "$proof/companion-tests"
unit companion-generated-build /usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -O2 \
    -I"$proof/companion-tests" "$proof/companion-tests/emitted_probe.cpp" -o "$proof/companion-tests/emitted-probe"
unit companion-generated-run "$proof/companion-tests/emitted-probe"
unit scalar-tests "$build/scalar-compiler/pipelang-scalar-tests" "$PWD" "$proof/sdk" "$proof/scalar-tests"
unit scalar-generated-build /usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -O2 \
    "$proof/scalar-tests/generated/runtime.cpp" -o "$proof/scalar-tests/generated/runtime-test"
unit scalar-generated-run "$proof/scalar-tests/generated/runtime-test"
if [ "${PIPELANG_NATIVE_TRACE_EXEC:-0}" = 1 ]; then
    PIPELANG_NATIVE_TRACE_EXEC=0
    for trace in "$proof"/units/*.exec; do
        name=$(basename "$trace" .exec)
        unit "$name-exec-audit" "$proof/sdk/bin/pipelang-sdk-tool" audit-exec --trace "$trace" --output "$trace.audit.json"
    done
fi
