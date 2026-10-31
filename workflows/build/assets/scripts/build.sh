#!/usr/bin/env sh
set -eu
platform_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$platform_directory/platform.sh"
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -z "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:-}" ]; then
    workspace=$(mktemp -d "${TMPDIR:-/tmp}/pipelang-native-build.XXXXXXXX")
    PIPELANG_NATIVE_TOOLCHAIN_PROOF="$workspace/proof"
fi
export PIPELANG_NATIVE_TOOLCHAIN_PROOF
PIPELANG_NATIVE_TRACE_EXEC=1
export PIPELANG_NATIVE_TRACE_EXEC
sh "$script_directory/native-toolchain.sh"
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
if [ "$build" != "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build" ]; then
    test ! -e "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build"
    ln -s "$build" "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build"
fi
if ! "$build/containedexec/pipelang-containment-probe" --suite "$build/containedexec/pipelang-contained" "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/installed-containment" > "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/installed-containment.log" 2>&1; then
    tail -c 8000 "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/installed-containment.log" >&2
    exit 1
fi
mkdir -p bin
# A successful build is the only operation allowed to replace this pointer.
test ! -e bin/native-current || test -L bin/native-current
ln -sfn "$PIPELANG_NATIVE_TOOLCHAIN_PROOF" bin/native-current
for command in pipelang pipelang-native pipelang-native-cpp pipelang-modelc pipelang-model-viewc pipelang-sdk-tool pipelang-contained pipelang-cpp-emitter pipelang-cpp-lexer pipelang-cpp-view-parser; do
    test ! -e "bin/$command" || test -L "bin/$command"
    ln -sfn "$PIPELANG_NATIVE_TOOLCHAIN_PROOF/sdk/bin/$command" "bin/$command"
done
printf 'Native SDK: %s/sdk\n' "$PIPELANG_NATIVE_TOOLCHAIN_PROOF"
