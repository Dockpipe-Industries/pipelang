#!/usr/bin/env sh
# Sourced by the default build. Native compilation stays inside a verified unit.
bootstrap="$PIPELANG_NATIVE_TOOLCHAIN_PROOF-bootstrap"
mkdir "$bootstrap"
/usr/bin/systemd-run --user --wait --pipe --collect \
    --unit="pipelang-source-bootstrap-$$" -p MemoryMax=1G -p MemoryHigh=700M \
    -p MemorySwapMax=0 -p TasksMax=128 -p RuntimeMaxSec=900 \
    -p KillMode=control-group -p TimeoutStopSec=2 \
    --working-directory="$PWD" --setenv="PATH=$PATH" \
    /usr/bin/strace -f -e trace=execve -s 4096 -o "$bootstrap/build.exec" \
    /usr/bin/sh "$script_directory/bootstrap-source-unit.sh" "$PWD" "$bootstrap" \
    > "$bootstrap/build.log" 2>&1
runner="$bootstrap/build/containedexec/pipelang-contained"
"$bootstrap/build/containedexec/pipelang-containment-probe" --suite "$runner" "$bootstrap/containment" \
    > "$bootstrap/containment.log" 2>&1
PIPELANG_NATIVE_RUNNER_SHA256=$(sha256sum "$runner" | cut -d' ' -f1)
export PIPELANG_NATIVE_RUNNER_SHA256
