#!/usr/bin/env sh
set -eu
if [ -n "${PIPELANG_NATIVE_TOOL_PATH:-}" ]; then
    PATH=$PIPELANG_NATIVE_TOOL_PATH
    export PATH
fi
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Select a completed native toolchain build}"
: "${PIPELANG_NATIVE_PACKAGE_PROOF:?Select a fresh absolute packaging proof directory}"
: "${PIPELANG_NATIVE_RUNNER:?Select the independently qualified absolute C++ runner}"
: "${PIPELANG_NATIVE_RUNNER_SHA256:?Pin the independently qualified runner SHA256}"
runner=$PIPELANG_NATIVE_RUNNER
test "$(sha256sum "$runner" | cut -d' ' -f1)" = "$PIPELANG_NATIVE_RUNNER_SHA256"
case "$PIPELANG_NATIVE_PACKAGE_PROOF/" in
    "$PWD/"*) echo 'Package proof must be outside sources' >&2; exit 1 ;;
    /*) ;;
    *) echo 'Package proof must be absolute' >&2; exit 1 ;;
esac
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
if [ "${1:-}" != --inside ]; then
    mkdir "$PIPELANG_NATIVE_PACKAGE_PROOF"
    exec "$runner" job --output "$PIPELANG_NATIVE_PACKAGE_PROOF/job" --estate "$PIPELANG_NATIVE_PACKAGE_PROOF" --timeout 3600 -- \
        /usr/bin/env PIPELANG_NATIVE_TOOLCHAIN_PROOF="$PIPELANG_NATIVE_TOOLCHAIN_PROOF" \
        PIPELANG_NATIVE_PACKAGE_PROOF="$PIPELANG_NATIVE_PACKAGE_PROOF" PIPELANG_NATIVE_BUILD_DIRECTORY="$build" \
        PIPELANG_NATIVE_RUNNER="$runner" PIPELANG_NATIVE_RUNNER_SHA256="$PIPELANG_NATIVE_RUNNER_SHA256" \
        PIPELANG_NATIVE_TOOL_PATH="${PIPELANG_NATIVE_TOOL_PATH:-}" sh "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Packaging requires verified aggregate containment}"
proof=$PIPELANG_NATIVE_PACKAGE_PROOF
DOCKPIPE_EVENT_LOG="$proof/operation-events.jsonl"
export DOCKPIPE_EVENT_LOG
sdk="$PIPELANG_NATIVE_TOOLCHAIN_PROOF/sdk"
tool="$sdk/bin/pipelang-sdk-tool"
manifest="$sdk/share/pipelang-model/sdk-manifest.json"
expected=$(sha256sum "$manifest" | cut -d' ' -f1)
mkdir "$proof/units"
unit() {
    unit_name=$1
    shift
    prefix="$proof/units/$unit_name"
    if "$runner" unit --output "$prefix" --estate "$proof" --timeout 900 -- \
        /usr/bin/strace -f -e trace=execve -s 4096 -o "$prefix.exec" "$@" > "$prefix.launcher.log" 2>&1; then
        echo "$unit_name: passed"
        return
    fi
    tail -c 6000 "$prefix.output" >&2
    tail -c 3000 "$prefix.launcher.log" >&2
    exit 1
}
unit archive "$tool" sdk-package --prefix "$sdk" --manifest "$manifest" --sha256 "$expected" --output "$proof/package"
unit independent-archive-reader /usr/bin/cmake -E tar tf "$proof/package/pipelang-native-sdk.tar"
package_manifest="$proof/package/sdk-package.json"
package_hash=$(sha256sum "$package_manifest" | cut -d' ' -f1)
unit extract "$tool" sdk-extract --manifest "$package_manifest" --sha256 "$package_hash" --output "$proof/relocated-sdk"
unit relocated-sdk-consumer "$build/sdk-tools/pipelang-sdk-tool-tests" "$PWD" "$proof/relocated-sdk" "$proof/consumer"
unit relocated-legacy-cli "$build/legacy-compiler/pipelang-legacy-compiler-tests" "$PWD" "$proof/legacy-cli" "$proof/relocated-sdk/bin/pipelang-native"
unit relocated-materializer "$build/legacy-compiler/pipelang-materialize-tests" "$proof/materializer" "$proof/relocated-sdk/bin/pipelang-native"
unit relocated-diagnostics "$build/legacy-compiler/pipelang-diagnostic-tests" "$proof/diagnostics" "$proof/relocated-sdk/bin/pipelang-native"
unit relocated-operations "$build/legacy-compiler/pipelang-operation-tests" "$proof/operations" "$proof/relocated-sdk/bin/pipelang-native"
unit relocated-scalars "$build/scalar-compiler/pipelang-scalar-tests" "$PWD" "$proof/relocated-sdk" "$proof/scalars"
unit relocated-scalar-build /usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -O2 "$proof/scalars/generated/runtime.cpp" -o "$proof/scalars/generated/test"
unit relocated-scalar-run "$proof/scalars/generated/test"
unit relocated-extended "$build/sdk-tools/pipelang-extended-tests" "$PWD" "$proof/relocated-sdk" "$proof/extended"
unit relocated-sdk-verify "$proof/relocated-sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/relocated-sdk" \
    --manifest "$proof/relocated-sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$expected"
for trace in "$proof"/units/*.exec; do
    audit_name=$(basename "$trace" .exec)
    "$runner" unit --output "$proof/units/$audit_name-exec-audit" --estate "$proof" --timeout 60 -- \
        "$tool" audit-exec --trace "$trace" --output "$trace.audit.json" > "$proof/units/$audit_name-exec-audit.launcher.log" 2>&1
done
