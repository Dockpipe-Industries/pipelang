#!/usr/bin/env sh
set -eu
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PIPELANG_NATIVE_TOOLCHAIN_PROOF=${PIPELANG_NATIVE_TOOLCHAIN_PROOF:-$(readlink -f bin/native-current)}
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
PIPELANG_NATIVE_RUNNER=${PIPELANG_NATIVE_RUNNER:-$build/containedexec/pipelang-contained}
PIPELANG_NATIVE_RUNNER_SHA256=${PIPELANG_NATIVE_RUNNER_SHA256:-$(sha256sum "$PIPELANG_NATIVE_RUNNER" | cut -d' ' -f1)}
if [ -z "${PIPELANG_NATIVE_CHECK_PROOF:-}" ]; then
    workspace=$(mktemp -d "${TMPDIR:-/tmp}/pipelang-native-check.XXXXXXXX")
    PIPELANG_NATIVE_CHECK_PROOF="$workspace/proof"
fi
export PIPELANG_NATIVE_TOOLCHAIN_PROOF PIPELANG_NATIVE_RUNNER PIPELANG_NATIVE_RUNNER_SHA256 PIPELANG_NATIVE_CHECK_PROOF
if [ -n "${PIPELANG_NATIVE_TOOL_PATH:-}" ]; then
    PATH=$PIPELANG_NATIVE_TOOL_PATH
    export PATH
fi
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Select the native build containing the C++ repository checker}"
: "${PIPELANG_NATIVE_CHECK_PROOF:?Select a fresh absolute repository-check proof}"
: "${PIPELANG_NATIVE_RUNNER:?Select the qualified native runner}"
: "${PIPELANG_NATIVE_RUNNER_SHA256:?Pin the qualified native runner SHA256}"
test "$(sha256sum "$PIPELANG_NATIVE_RUNNER" | cut -d' ' -f1)" = "$PIPELANG_NATIVE_RUNNER_SHA256"
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
proof=$PIPELANG_NATIVE_CHECK_PROOF
case "$proof/" in
    "$PWD/"*) echo 'Check proof must be outside sources' >&2; exit 1 ;;
    /*) ;;
    *) echo 'Check proof must be absolute' >&2; exit 1 ;;
esac
if [ "${1:-}" != --inside ]; then
    mkdir "$proof"
    exec "$PIPELANG_NATIVE_RUNNER" job --output "$proof/job" --estate "$proof" --timeout 900 -- \
        /usr/bin/env PIPELANG_CHECK_NATIVE_TOOLCHAIN=1 PIPELANG_NATIVE_TOOLCHAIN_PROOF="$PIPELANG_NATIVE_TOOLCHAIN_PROOF" \
        PIPELANG_NATIVE_BUILD_DIRECTORY="$build" PIPELANG_NATIVE_CHECK_PROOF="$proof" \
        PIPELANG_NATIVE_RUNNER="$PIPELANG_NATIVE_RUNNER" PIPELANG_NATIVE_RUNNER_SHA256="$PIPELANG_NATIVE_RUNNER_SHA256" \
        PIPELANG_NATIVE_TOOL_PATH="${PIPELANG_NATIVE_TOOL_PATH:-}" DOCKPIPE_BIN="$DOCKPIPE_BIN" sh "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Repository checks require verified aggregate containment}"
"$PIPELANG_NATIVE_RUNNER" unit --output "$proof/check" --estate "$proof" --timeout 600 -- \
    /usr/bin/strace -f -e trace=execve -s 4096 -o "$proof/check.exec" \
    "$build/sdk-tools/pipelang-repository-check" --source-root "$PWD" --dockpipe "$DOCKPIPE_BIN" \
    --output "$proof/contracts" > "$proof/check.launcher.log" 2>&1
"$PIPELANG_NATIVE_RUNNER" unit --output "$proof/exec-audit" --estate "$proof" --timeout 60 -- \
    "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/sdk/bin/pipelang-sdk-tool" audit-exec --trace "$proof/check.exec" --output "$proof/check.exec.audit.json" \
    > "$proof/exec-audit.launcher.log" 2>&1
