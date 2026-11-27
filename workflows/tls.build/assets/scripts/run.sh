#!/usr/bin/env bash
set -euo pipefail
: "${PIPELANG_TLS_SDK_PREFIX:?Select the exact installed compiler SDK}"
: "${PIPELANG_NATIVE_RESOLVER_PREFIX:?Select the installed native dependency resolver}"
: "${PIPELANG_NATIVE_TOOLCHAIN:?Select the explicit Rust toolchain manifest}"
: "${PIPELANG_NATIVE_TOOLCHAIN_SHA256:?Pin the Rust toolchain manifest}"
: "${PIPELANG_NATIVE_CACHE:?Select the prepared offline dependency cache}"
: "${PIPELANG_RUSTLS_ARCHIVE:?Select the pinned upstream source archive}"
: "${PIPELANG_TLS_PROOF:?Select a fresh absolute proof path}"
runner="$PIPELANG_TLS_SDK_PREFIX/bin/pipelang-contained"
expected=19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == "$expected" ]]
[[ "$PIPELANG_TLS_PROOF" == /* ]]
if [[ "${1:-}" != --inside ]]; then
    mkdir "$PIPELANG_TLS_PROOF"
    exec "$runner" job --output "$PIPELANG_TLS_PROOF/job" --estate "$PIPELANG_TLS_PROOF" --timeout 600 -- \
        /usr/bin/env PIPELANG_TLS_SDK_PREFIX="$PIPELANG_TLS_SDK_PREFIX" \
        PIPELANG_NATIVE_RESOLVER_PREFIX="$PIPELANG_NATIVE_RESOLVER_PREFIX" \
        PIPELANG_NATIVE_TOOLCHAIN="$PIPELANG_NATIVE_TOOLCHAIN" PIPELANG_NATIVE_TOOLCHAIN_SHA256="$PIPELANG_NATIVE_TOOLCHAIN_SHA256" \
        PIPELANG_NATIVE_CACHE="$PIPELANG_NATIVE_CACHE" PIPELANG_RUSTLS_ARCHIVE="$PIPELANG_RUSTLS_ARCHIVE" PIPELANG_TLS_PROOF="$PIPELANG_TLS_PROOF" \
        /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Verified containment is required}"
mkdir "$PIPELANG_TLS_PROOF/units"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$PIPELANG_TLS_PROOF/units/$name" --estate "$PIPELANG_TLS_PROOF" --timeout 300 -- "$@"
}
printf '%s  %s\n' db881d08ccfd7fde80717bc752c50b9bafb2a06ed80c7c9640d6a236aaf69819 "$PIPELANG_RUSTLS_ARCHIVE" | sha256sum -c -
mkdir "$PIPELANG_TLS_PROOF/source"
tar -xzf "$PIPELANG_RUSTLS_ARCHIVE" -C "$PIPELANG_TLS_PROOF/source" --strip-components=1
unit configure /usr/bin/cmake -S modules/tls/rustls -B "$PIPELANG_TLS_PROOF/build" \
    -DCMAKE_PREFIX_PATH="$PIPELANG_NATIVE_RESOLVER_PREFIX" \
    -DPIPELANG_NATIVE_TOOLCHAIN="$PIPELANG_NATIVE_TOOLCHAIN" \
    -DPIPELANG_NATIVE_TOOLCHAIN_SHA256="$PIPELANG_NATIVE_TOOLCHAIN_SHA256" \
    -DPIPELANG_NATIVE_CACHE="$PIPELANG_NATIVE_CACHE" -DPIPELANG_RUSTLS_SOURCE="$PIPELANG_TLS_PROOF/source" -DCMAKE_INSTALL_PREFIX="$PIPELANG_TLS_PROOF/install"
unit build /usr/bin/cmake --build "$PIPELANG_TLS_PROOF/build" --parallel 1
unit native-boundary "$PIPELANG_TLS_PROOF/build/rustls-native-boundary"
unit install /usr/bin/cmake --install "$PIPELANG_TLS_PROOF/build"
cp "$PIPELANG_TLS_PROOF/build/native-dependencies/pipelang_rustls/build-receipt.json" "$PIPELANG_TLS_PROOF/install/build-receipt.json"
(cd "$PIPELANG_TLS_PROOF/install" && sha256sum include/rustls.h lib/librustls_ffi.a Cargo.lock > SHA256SUMS)
