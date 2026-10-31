#!/usr/bin/env sh
set -eu
platform_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$platform_directory/../../../build/assets/scripts/platform.sh"
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PIPELANG_NATIVE_TOOLCHAIN_PROOF=${PIPELANG_NATIVE_TOOLCHAIN_PROOF:-$(readlink -f bin/native-current)}
: "${PIPELANG_NATIVE_TOOLCHAIN_PROOF:?Run the build workflow first}"
if [ -z "${PIPELANG_NATIVE_COMPAT_PROOF:-}" ]; then
    workspace=$(mktemp -d "${TMPDIR:-/tmp}/pipelang-native-test.XXXXXXXX")
    PIPELANG_NATIVE_COMPAT_PROOF="$workspace/proof"
fi
export PIPELANG_NATIVE_TOOLCHAIN_PROOF PIPELANG_NATIVE_COMPAT_PROOF
exec sh "$script_directory/native-toolchain.sh"
