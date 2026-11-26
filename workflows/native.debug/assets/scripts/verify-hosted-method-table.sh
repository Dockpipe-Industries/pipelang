#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail
sdk=$1
compiler_record=$2
llc=$3
include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
driver="$assets/HostedWideMethods.cpp"
cxx=$(<"$compiler_record")

mkdir "$proof"
source="$proof/WideMethods.pipe"
members=()
for ((index=0; index<256; ++index)); do
    printf -v name 'Method%03d' "$index"
    members+=("$name")
done
write_methods() {
    local first=$1 last=$2
    local index
    for ((index=first; index<=last; ++index)); do
        printf '    public pure int %s() => %d;\n' "${members[index]}" "$index"
    done
}
{
    printf 'module WideMethods;\n\npublic Class Table {\n'
    write_methods 0 255
    printf '}\n'
} > "$source"
options=()
for method in "${members[@]:1:255}"; do
    options+=(--host-method "$method")
done

"$cxx" -std=c++20 -O2 -fPIE -pthread -Wall -Wextra -Wpedantic -Werror -I "$include" \
    -c "$driver" -o "$proof/driver.o"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry WideMethods.Table.Method000 --host-symbol proof_wide_methods \
        "${options[@]}" --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$sdk/libpipelang_native_app_runtime.a" \
        -o "$proof/$profile"
    env -i "$proof/$profile"
done

sha256sum "$proof/release.o" > "$proof/published.sha256"
if "$sdk/pipelang-objectc" --source "$source" --entry WideMethods.Table.Method000 --host-symbol proof_wide_methods \
    "${options[@]}" --host-method "${members[255]}" --profile release --llc "$llc" --output "$proof/release.o" \
    > "$proof/overflow.result" 2>&1; then
    exit 1
fi
grep -F 'native hosted model exceeds the method count limit' "$proof/overflow.result"
sha256sum --check --status "$proof/published.sha256"
