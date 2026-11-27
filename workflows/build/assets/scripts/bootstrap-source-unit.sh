#!/usr/bin/env sh
# Minimal source bootstrap: one capped unit, no generated program execution.
set -eu
root=$1
output=$2
group="/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)"
test "$(cat "$group/memory.max")" = 1073741824
test "$(cat "$group/memory.high")" = 734003200
test "$(cat "$group/memory.swap.max")" = 0
test "$(cat "$group/pids.max")" = 128
coordinator=$$
trap 'exit 143' TERM INT
(
    while test -d "$group"; do
        if test "$(cat "$group/memory.current")" -ge 838860800; then
            kill -TERM "$coordinator"
            exit 1
        fi
        sleep 0.1
    done
) &
monitor=$!
trap 'kill "$monitor" 2>/dev/null || true' EXIT
/usr/bin/cmake -S "$root/src/native/bootstrap" -B "$output/build" -DCMAKE_BUILD_TYPE=Release
/usr/bin/cmake --build "$output/build" --parallel 1
