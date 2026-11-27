#!/bin/sh
# First-stage source bootstrap uses sampled RSS, not a Linux kernel hard cap.
set -eu
bootstrap="$PIPELANG_NATIVE_TOOLCHAIN_PROOF-bootstrap"
test ! -e "$bootstrap"
mkdir -p "$bootstrap"
# A separate job-control process group is the only signal target. Never signal
# PIDs obtained from a system-wide inventory.
set -m
(
    ulimit -c 0
    ulimit -f 131072
    "$PIPELANG_CMAKE" -S "$PWD/src/native/bootstrap" -B "$bootstrap/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$PIPELANG_CXX"
    "$PIPELANG_CMAKE" --build "$bootstrap/build" --parallel 1
) > "$bootstrap/build.log" 2>&1 &
bootstrap_pid=$!
bootstrap_group=$(ps -p "$bootstrap_pid" -o pgid= | tr -d ' ')
if [ "$bootstrap_group" != "$bootstrap_pid" ]; then
    kill -TERM "$bootstrap_pid" 2>/dev/null || true
    wait "$bootstrap_pid" 2>/dev/null || true
    echo 'macOS bootstrap requires its own process group' >&2
    exit 1
fi
bootstrap_stop() {
    kill -KILL -- "-$bootstrap_pid" 2>/dev/null || true
    wait "$bootstrap_pid" 2>/dev/null || true
}
trap 'bootstrap_stop; exit 1' INT TERM HUP
bootstrap_start=$(date +%s)
bootstrap_failure=""
while kill -0 "$bootstrap_pid" 2>/dev/null; do
    bootstrap_rss=$(ps -axo pid=,ppid=,rss= | awk -v root="$bootstrap_pid" '
        { parent[$1] = $2; memory[$1] = $3 }
        END {
            owned[root] = 1
            for (pass = 0; pass < 128; pass++)
                for (pid in parent)
                    if (parent[pid] in owned) owned[pid] = 1
            total = 0
            for (pid in owned) total += memory[pid]
            printf "%.0f\n", total
        }')
    bootstrap_elapsed=$(($(date +%s) - bootstrap_start))
    if [ "$bootstrap_rss" -ge 819200 ] || [ "$bootstrap_elapsed" -ge 900 ]; then
        bootstrap_failure='macOS bootstrap sampled memory/deadline stop'
        break
    fi
    sleep 0.1
done
if [ -n "$bootstrap_failure" ]; then
    bootstrap_stop
    echo "$bootstrap_failure" >&2
    exit 1
fi
if ! wait "$bootstrap_pid"; then
    tail -c 6000 "$bootstrap/build.log" >&2
    exit 1
fi
trap - INT TERM HUP
runner="$bootstrap/build/containedexec/pipelang-contained"
PIPELANG_NATIVE_RUNNER_SHA256=$(sha256sum "$runner" | cut -d' ' -f1)
export PIPELANG_NATIVE_RUNNER_SHA256
if ! "$bootstrap/build/containedexec/pipelang-containment-probe" --suite "$runner" "$bootstrap/admission" > "$bootstrap/admission.log" 2>&1; then
    tail -c 8000 "$bootstrap/admission.log" >&2
    exit 1
fi
