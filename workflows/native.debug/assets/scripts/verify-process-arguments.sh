#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
gdb=$3
proof=$4

assets=$(cd "$(dirname "$0")/.." && pwd -P)
source="$assets/ProcessArguments.pipe"
entry=NativeProof.ProcessArguments.App
arguments=('' 'two words' --source-trace '你好')
mkdir "$proof"

check_void_result() {
    local app=$1
    local input=$2
    local expected_status=$3
    local expected_message=$4
    local status=0
    local output
    env -i "$app" "$input" > "$proof/void.stdout" 2> "$proof/void.stderr" || status=$?
    test "$status" -eq "$expected_status"
    test ! -s "$proof/void.stdout"
    output=$(<"$proof/void.stderr")
    test "$output" = "$expected_message"
}

for profile in debug release; do
    app="$proof/arguments-$profile"
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" "$entry.Run" "$app" "$profile"
    test "$(env -i "$app")" = 17
    test "$(env -i "$app" "${arguments[@]}")" = 42
    test "$(env -i "$app" --cancel)" = -1

    status=0
    env -i "$app" "$(printf '\377')" > "$proof/$profile-invalid.stdout" \
        2> "$proof/$profile-invalid.stderr" || status=$?
    test "$status" -eq 1
    test ! -s "$proof/$profile-invalid.stdout"
    grep -Fi 'UTF-8' "$proof/$profile-invalid.stderr"

    resumed="$proof/resumed-$profile"
    /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" "$entry.Resume" "$resumed" "$profile"
    test "$(env -i "$resumed" --cancel 'mañana')" = 42

    fault="$proof/fault-$profile"
    "$sdk/pipelang-objectc" --executable --target linux-x64 --profile "$profile" --source-trace \
        --source "$source" --entry "$entry.Fault" --llc "$llc" --linker /usr/bin/ld \
        --link-sdk "$sdk" --link-sdk-sha256 "$(<"$sdk/link-sdk.sha256")" --output "$fault"
    test "$(env -i "$fault")" = 17
    status=0
    env -i "$fault" command > "$proof/$profile-fault.stdout" 2> "$proof/$profile-fault.stderr" || status=$?
    test "$status" -eq 1
    test ! -s "$proof/$profile-fault.stdout"
    grep -Fx 'argument-triggered source fault' "$proof/$profile-fault.stderr"
    grep -Fx "  at $entry.Fault (ProcessArguments.pipe:32)" "$proof/$profile-fault.stderr"

    for method in Run ResultRun AsyncRun; do
        quiet="$proof/quiet-$method-$profile"
        /bin/bash "$assets/scripts/build-runtime-app.sh" "$sdk" "$llc" "$source" \
            "NativeProof.ProcessArguments.QuietApp.$method" "$quiet" "$profile"
        check_void_result "$quiet" 1 0 ""
        if [[ $method == Run ]]; then
            check_void_result "$quiet" 0 0 ""
            check_void_result "$quiet" -1 1 'void source failed'
        else
            check_void_result "$quiet" 0 2 'void returned cancellation'
            check_void_result "$quiet" -1 1 'void returned failure'
        fi
    done
done

"$gdb" --batch --nx --quiet -ex "directory $assets" -ex 'set disable-randomization off' \
    -ex 'tbreak ProcessArguments.pipe:6' -ex 'run "" "two words" --source-trace "你好"' \
    -ex 'print arguments.count' -ex continue "$proof/arguments-debug" > "$proof/debugger.output" 2>&1
grep -F 'at ProcessArguments.pipe:6' "$proof/debugger.output"
grep -Fx '$1 = 4' "$proof/debugger.output"
grep -F 'exited normally' "$proof/debugger.output"
