#!/usr/bin/env sh
set -eu
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PIPELANG_NATIVE_TOOLCHAIN_PROOF=${PIPELANG_NATIVE_TOOLCHAIN_PROOF:-$(readlink -f bin/native-current)}
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Run the build workflow first}"
build=${PIPELANG_NATIVE_BUILD_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF/build}
PIPELANG_NATIVE_RUNNER=${PIPELANG_NATIVE_RUNNER:-$build/containedexec/pipelang-contained}
PIPELANG_NATIVE_RUNNER_SHA256=${PIPELANG_NATIVE_RUNNER_SHA256:-$(sha256sum "$PIPELANG_NATIVE_RUNNER" | cut -d' ' -f1)}
if [ -z "${PIPELANG_NATIVE_PACKAGE_PROOF:-}" ]; then
    workspace=$(mktemp -d "${TMPDIR:-/tmp}/pipelang-native-package.XXXXXXXX")
    PIPELANG_NATIVE_PACKAGE_PROOF="$workspace/proof"
fi
export PIPELANG_NATIVE_TOOLCHAIN_PROOF PIPELANG_NATIVE_PACKAGE_PROOF PIPELANG_NATIVE_RUNNER PIPELANG_NATIVE_RUNNER_SHA256
sh "$script_directory/native-sdk.sh"
mkdir -p bin
test ! -e bin/native-package || test -L bin/native-package
ln -sfn "$PIPELANG_NATIVE_PACKAGE_PROOF/package" bin/native-package
printf 'Native package: %s/package\n' "$PIPELANG_NATIVE_PACKAGE_PROOF"
