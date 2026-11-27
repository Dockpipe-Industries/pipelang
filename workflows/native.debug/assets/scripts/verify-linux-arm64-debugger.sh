#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ "${1:-}" != --isolated ]]; then
    exec /usr/bin/unshare --user --map-root-user --net /bin/bash "$0" --isolated "$@"
fi
shift

qemu=$1
lldb=$2
app=$3
sources=$4
output=$5
port=23125
printf -v proc_port ':%04X$' "$port"

# Older QEMU builds listen on all interfaces, so expose only a private loopback device.
/usr/sbin/ip link set lo up
"$qemu" -g "$port" "$app" 1 --require-suspension >"$output.app" &
guest=$!
trap 'kill "$guest" 2>/dev/null || true' EXIT
for attempt in {1..100}; do
    if awk -v port="$proc_port" '$2 ~ port && $4 == "0A" {found=1} END {exit !found}' /proc/net/tcp; then
        break
    fi
    sleep .05
done

"$lldb" -b -x \
    -o "target create --arch aarch64 \"$app\"" \
    -o "settings set target.source-map /pipelang-source \"$sources\"" \
    -o "gdb-remote 127.0.0.1:$port" \
    -o 'breakpoint set --file SuspendingCollections.pipe --line 12' \
    -o 'process continue' -o 'frame variable acquired' -o bt -o 'process continue' >"$output"
wait "$guest"
trap - EXIT
grep -F 'stop reason = breakpoint' "$output"
grep -F 'SuspendingCollections.App.Read(index=1) at SuspendingCollections.pipe:12' "$output"
grep -Fx '(bool) acquired = false' "$output"
grep -F 'exited with status = 0' "$output"
grep -Fx '42' "$output.app"
