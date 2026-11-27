#!/bin/sh
# Platform tools shared by the declared build, check, test and package workflows.
case "$(uname -s)" in
    Linux)
        PIPELANG_NATIVE_PLATFORM=linux
        PIPELANG_CMAKE=${PIPELANG_CMAKE:-/usr/bin/cmake}
        PIPELANG_CXX=${PIPELANG_CXX:-/usr/bin/c++}
        export PIPELANG_CMAKE PIPELANG_CXX
        ;;
    Darwin)
        PIPELANG_NATIVE_PLATFORM=macos
        TMPDIR=$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)
        export TMPDIR
        for proof_variable in PIPELANG_NATIVE_TOOLCHAIN_PROOF PIPELANG_NATIVE_COMPAT_PROOF PIPELANG_NATIVE_CHECK_PROOF PIPELANG_NATIVE_PACKAGE_PROOF; do
            case "$proof_variable" in
                PIPELANG_NATIVE_TOOLCHAIN_PROOF) proof_path=${PIPELANG_NATIVE_TOOLCHAIN_PROOF:-} ;;
                PIPELANG_NATIVE_COMPAT_PROOF) proof_path=${PIPELANG_NATIVE_COMPAT_PROOF:-} ;;
                PIPELANG_NATIVE_CHECK_PROOF) proof_path=${PIPELANG_NATIVE_CHECK_PROOF:-} ;;
                PIPELANG_NATIVE_PACKAGE_PROOF) proof_path=${PIPELANG_NATIVE_PACKAGE_PROOF:-} ;;
            esac
            if [ -n "$proof_path" ]; then
                proof_parent=$(CDPATH= cd -- "$(dirname "$proof_path")" && pwd -P)
                export "$proof_variable=$proof_parent/$(basename "$proof_path")"
            fi
        done
        PIPELANG_CMAKE=${PIPELANG_CMAKE:-$(command -v cmake)}
        PIPELANG_CXX=${PIPELANG_CXX:-/usr/bin/clang++}
        export PIPELANG_CMAKE PIPELANG_CXX
        sha256sum() { "$PIPELANG_CMAKE" -E sha256sum "$@"; }
        readlink() { /usr/bin/readlink "$@"; }
        ;;
    *) echo 'No qualified native workflow backend for this host' >&2; exit 1 ;;
esac
export PIPELANG_NATIVE_PLATFORM

run_native_job() {
    native_job_runner=$1
    native_job_prefix=$2
    shift 2
    if "$native_job_runner" job --output "$native_job_prefix" "$@"; then
        return 0
    else
        native_job_result=$?
        if [ -f "$native_job_prefix.output" ]; then
            tail -c 12000 "$native_job_prefix.output" >&2
        fi
        return "$native_job_result"
    fi
}
