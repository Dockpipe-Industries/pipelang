#!/usr/bin/env bash
set -euo pipefail
: "${PIPELANG_CRYPTO_SDK_PREFIX:?Select the exact installed compiler SDK}"
: "${PIPELANG_NATIVE_RESOLVER_PREFIX:?Select the installed native dependency resolver}"
: "${PIPELANG_NATIVE_TOOLCHAIN:?Select the explicit Rust toolchain manifest}"
: "${PIPELANG_NATIVE_TOOLCHAIN_SHA256:?Pin the Rust toolchain manifest}"
: "${PIPELANG_NATIVE_CACHE:?Select the prepared offline dependency cache}"
: "${PIPELANG_CRYPTO_PROOF:?Select a fresh absolute proof path}"
runner="$PIPELANG_CRYPTO_SDK_PREFIX/bin/pipelang-contained"
expected=19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == "$expected" ]]
[[ "$PIPELANG_CRYPTO_PROOF" == /* ]]
if [[ "${1:-}" != --inside ]]; then
    mkdir "$PIPELANG_CRYPTO_PROOF"
    exec "$runner" job --output "$PIPELANG_CRYPTO_PROOF/job" --estate "$PIPELANG_CRYPTO_PROOF" --timeout 600 -- \
        /usr/bin/env PIPELANG_CRYPTO_SDK_PREFIX="$PIPELANG_CRYPTO_SDK_PREFIX" \
        PIPELANG_NATIVE_RESOLVER_PREFIX="$PIPELANG_NATIVE_RESOLVER_PREFIX" \
        PIPELANG_NATIVE_TOOLCHAIN="$PIPELANG_NATIVE_TOOLCHAIN" PIPELANG_NATIVE_TOOLCHAIN_SHA256="$PIPELANG_NATIVE_TOOLCHAIN_SHA256" \
        PIPELANG_NATIVE_CACHE="$PIPELANG_NATIVE_CACHE" PIPELANG_CRYPTO_PROOF="$PIPELANG_CRYPTO_PROOF" \
        /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Verified containment is required}"
mkdir "$PIPELANG_CRYPTO_PROOF/units"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$PIPELANG_CRYPTO_PROOF/units/$name" --estate "$PIPELANG_CRYPTO_PROOF" --timeout 300 -- "$@"
}
unit configure /usr/bin/cmake -S modules/crypto -B "$PIPELANG_CRYPTO_PROOF/build" \
    -DCMAKE_PREFIX_PATH="$PIPELANG_NATIVE_RESOLVER_PREFIX" \
    -DPIPELANG_NATIVE_TOOLCHAIN="$PIPELANG_NATIVE_TOOLCHAIN" \
    -DPIPELANG_NATIVE_TOOLCHAIN_SHA256="$PIPELANG_NATIVE_TOOLCHAIN_SHA256" \
    -DPIPELANG_NATIVE_CACHE="$PIPELANG_NATIVE_CACHE" -DCMAKE_INSTALL_PREFIX="$PIPELANG_CRYPTO_PROOF/install"
unit tests /usr/bin/cmake --build "$PIPELANG_CRYPTO_PROOF/build" --target pipelang_crypto_test --parallel 1
unit build /usr/bin/cmake --build "$PIPELANG_CRYPTO_PROOF/build" --parallel 1
unit native-boundary "$PIPELANG_CRYPTO_PROOF/build/crypto-native-boundary"
unit install /usr/bin/cmake --install "$PIPELANG_CRYPTO_PROOF/build"
cp "$PIPELANG_CRYPTO_PROOF/build/native-dependencies/pipelang_crypto/build-receipt.json" "$PIPELANG_CRYPTO_PROOF/install/build-receipt.json"
(cd "$PIPELANG_CRYPTO_PROOF/install" && sha256sum include/pipelang_crypto.h lib/libpipelang_crypto.a Cargo.lock > SHA256SUMS)
