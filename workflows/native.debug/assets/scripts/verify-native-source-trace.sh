#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
proof=$3
assets=$(cd "$(dirname "$0")/.." && pwd -P)
mkdir "$proof"
args=(--source "$assets/Trace.pipe" --entry NativeProof.Trace.App.Run --llc "$llc"
    --executable --linker /usr/bin/ld --link-sdk "$sdk" --link-sdk-sha256 "$(<"$sdk/link-sdk.sha256")")

cat > "$proof/expected-sync" <<'EOF'
integer division by zero
  at NativeProof.Trace.Math.Divide (Trace.pipe:6)
  at NativeProof.Trace.Math.Relay (Trace.pipe:10)
  at NativeProof.Trace.App.Run (Trace.pipe:22)
EOF
cat > "$proof/expected-async" <<'EOF'
integer division by zero
  at NativeProof.Trace.Math.Divide (Trace.pipe:6)
  at NativeProof.Trace.Math.Relay (Trace.pipe:10)
  at NativeProof.Trace.App.Child (Trace.pipe:55)
  at NativeProof.Trace.App.Run (Trace.pipe:31)
EOF

for profile in debug release; do
    app="$proof/$profile"
    "$sdk/pipelang-objectc" "${args[@]}" --profile "$profile" --source-trace --output "$app"
    status=0
    env -i "$app" 0 --source-trace > "$proof/$profile.sync" 2>&1 || status=$?
    test "$status" -eq 1
    cmp "$proof/expected-sync" "$proof/$profile.sync"
    status=0
    env -i "$app" 3 --source-trace > "$proof/$profile.async" 2>&1 || status=$?
    test "$status" -eq 1
    cmp "$proof/expected-async" "$proof/$profile.async"

    status=0
    failure=$(env -i "$app" 0 2>&1) || status=$?
    [[ $status == 1 && "$failure" == "integer division by zero" ]]
    status=0
    handled=$(env -i "$app" 1 --source-trace 2>&1) || status=$?
    [[ $status == 1 && "$handled" == "handled integer division by zero" ]]
    status=0
    discarded=$(env -i "$app" 4 --source-trace 2>&1) || status=$?
    [[ $status == 1 && "$discarded" == "integer division by zero" ]]
    status=0
    env -i "$app" 5 --source-trace > "$proof/$profile.optional" 2>&1 || status=$?
    [[ $status == 1 ]]
    grep -Fx 'Optional.Value requires Some' "$proof/$profile.optional"
    grep -Fx '  at NativeProof.Trace.App.Run (Trace.pipe:41)' "$proof/$profile.optional"
    status=0
    env -i "$app" 6 --source-trace > "$proof/$profile.result" 2>&1 || status=$?
    [[ $status == 1 ]]
    grep -Fx 'Result.Value requires Success' "$proof/$profile.result"
    grep -Fx '  at NativeProof.Trace.App.Run (Trace.pipe:45)' "$proof/$profile.result"
    [[ $(env -i "$app" 2 --source-trace) == 42 ]]
done

"$sdk/pipelang-objectc" "${args[@]}" --output "$proof/without-traces"
status=0
unavailable=$(env -i "$proof/without-traces" 0 --source-trace 2>&1) || status=$?
[[ $status == 64 && "$unavailable" == "source trace metadata is unavailable. Recompile with --source-trace" ]]
cat "$proof/release.async"
