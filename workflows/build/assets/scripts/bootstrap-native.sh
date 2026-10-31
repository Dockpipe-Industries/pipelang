#!/usr/bin/env sh
# Sourced by the native build lane. The seed is a checksum-pinned native SDK
# archive produced/admitted separately; this script never compiles outside caps.
: "${PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE:?Select a checksum-pinned native SDK archive or an already qualified runner}"
: "${PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE_SHA256:?Pin the native SDK archive SHA256}"
bootstrap=${PIPELANG_NATIVE_BOOTSTRAP_DIRECTORY:-$PIPELANG_NATIVE_TOOLCHAIN_PROOF-bootstrap}
case "$bootstrap/" in
    "$PWD/"*) echo 'Native bootstrap must be outside sources' >&2; exit 1 ;;
    /*) ;;
    *) echo 'Native bootstrap directory must be absolute' >&2; exit 1 ;;
esac
test "$(realpath -m "$bootstrap")" = "$bootstrap"
test -f "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE"
test ! -L "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE"
test "$(stat -c %s "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE")" -le 134217728
mkdir "$bootstrap"
cp "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE" "$bootstrap/seed.tar"
test "$(sha256sum "$bootstrap/seed.tar" | cut -d' ' -f1)" = "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE_SHA256"
chmod 0444 "$bootstrap/seed.tar"
mkdir "$bootstrap/sdk"
/usr/bin/strace -f -e trace=execve -s 4096 -o "$bootstrap/extract.exec" \
    /usr/bin/cmake -E chdir "$bootstrap/sdk" /usr/bin/cmake -E tar xf "$bootstrap/seed.tar"
test "$(sha256sum "$bootstrap/seed.tar" | cut -d' ' -f1)" = "$PIPELANG_NATIVE_BOOTSTRAP_ARCHIVE_SHA256"
seed_manifest="$bootstrap/sdk/share/pipelang-model/sdk-manifest.json"
seed_hash=$(sha256sum "$seed_manifest" | cut -d' ' -f1)
"$bootstrap/sdk/bin/pipelang-sdk-tool" verify --prefix "$bootstrap/sdk" --manifest "$seed_manifest" --sha256 "$seed_hash"
"$bootstrap/sdk/bin/pipelang-sdk-tool" audit-exec --trace "$bootstrap/extract.exec" --output "$bootstrap/extract.exec.audit.json"
runner="$bootstrap/sdk/bin/pipelang-contained"
PIPELANG_NATIVE_RUNNER_SHA256=$(sha256sum "$runner" | cut -d' ' -f1)
export PIPELANG_NATIVE_RUNNER_SHA256
