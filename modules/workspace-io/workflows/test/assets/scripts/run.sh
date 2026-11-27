#!/usr/bin/env bash
set -euo pipefail
: "${PIPELANG_SDK_PREFIX:?Select the exact installed SDK}"
: "${PIPELANG_WORKSPACE_CRYPTO_PREFIX:?Select the pinned installed crypto capability}"
: "${PIPELANG_WORKSPACE_IO_PROOF:?Select an absolute proof directory}"
module_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
runner="$PIPELANG_SDK_PREFIX/bin/pipelang-contained"
verify() {
    [[ "$(sha256sum "$1" | cut -d' ' -f1)" == "$2" ]]
}
verify "$runner" 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173
verify "$PIPELANG_SDK_PREFIX/share/pipelang-model/sdk-manifest.json" 200890fd5a451f7e1f1665104dcedb8cce4491fac9c09a2f3d98b51bec6c5281
verify "$PIPELANG_WORKSPACE_CRYPTO_PREFIX/include/pipelang_crypto.h" 798d0e7fcae263ac06a12d62fd8d817d7bdffc6afadccd74c3fe6be2904028f0
verify "$PIPELANG_WORKSPACE_CRYPTO_PREFIX/lib/libpipelang_crypto.a" d94a241d22783db6bf3d36d429e5942c339ff610ded4122b32a3afe64ee858c8
proof="$PIPELANG_WORKSPACE_IO_PROOF"
mkdir -p "$proof"
if [[ "${1:-}" != --inside ]]; then
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 600 -- \
        /usr/bin/env PIPELANG_SDK_PREFIX="$PIPELANG_SDK_PREFIX" \
        PIPELANG_WORKSPACE_CRYPTO_PREFIX="$PIPELANG_WORKSPACE_CRYPTO_PREFIX" \
        PIPELANG_WORKSPACE_IO_PROOF="$proof" PIPELANG_WORKSPACE_IO_SANITIZE="${PIPELANG_WORKSPACE_IO_SANITIZE:-OFF}" \
        /bin/bash "$module_root/workflows/test/assets/scripts/run.sh" --inside
fi
: "${CONTAINED_JOB_SLICE:?Verified aggregate containment required}"
unit() {
    local name="$1"
    shift
    if "$runner" unit --output "$proof/$name" --estate "$proof" --timeout 180 -- "$@"; then
        return
    fi
    tail -c 6000 "$proof/$name.output" >&2
    exit 1
}
unit configure /usr/bin/cmake -S "$module_root" -B "$proof/build" \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG' \
    -DCMAKE_PREFIX_PATH="$PIPELANG_SDK_PREFIX" -DCMAKE_INSTALL_PREFIX="$proof/install" \
    -DPIPELANG_WORKSPACE_CRYPTO_PREFIX="$PIPELANG_WORKSPACE_CRYPTO_PREFIX" \
    -DPIPELANG_WORKSPACE_CRYPTO_HEADER_SHA256=798d0e7fcae263ac06a12d62fd8d817d7bdffc6afadccd74c3fe6be2904028f0 \
    -DPIPELANG_WORKSPACE_CRYPTO_LIBRARY_SHA256=d94a241d22783db6bf3d36d429e5942c339ff610ded4122b32a3afe64ee858c8 \
    -DPIPELANG_WORKSPACE_IO_SANITIZE="${PIPELANG_WORKSPACE_IO_SANITIZE:-OFF}"
unit build /usr/bin/cmake --build "$proof/build" --parallel 1
unit test /usr/bin/ctest --test-dir "$proof/build" --output-on-failure
unit install /usr/bin/cmake --install "$proof/build"
unit consumer-configure /usr/bin/cmake -S "$module_root/tests/consumer" -B "$proof/consumer" \
    "-DCMAKE_PREFIX_PATH=$proof/install;$PIPELANG_SDK_PREFIX"
unit consumer-build /usr/bin/cmake --build "$proof/consumer" --parallel 1
unit consumer /usr/bin/env ASAN_OPTIONS=detect_leaks=1 "$proof/consumer/installed-consumer"
