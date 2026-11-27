#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
archive=$2
sdk=$3
sdk_cxx=$(<"$4")
sdk_tool=$5
test -x "$sdk_cxx"
mkdir "$sdk"
install -m 0755 "$compiler" "$sdk/pipelang-objectc"
install -m 0644 "$archive" "$sdk/libpipelang_native_app_runtime.a"

# Resolve the startup and static libraries only while building the SDK.
# Application links consume the copied files and never invoke this compiler.
inputs=(crt1.o crti.o crtbeginT.o crtend.o crtn.o libstdc++.a libgcc.a libgcc_eh.a libc.a)
for input in "${inputs[@]}"; do
    resolved=$("$sdk_cxx" "-print-file-name=$input")
    if [[ "$resolved" != /* || ! -f "$resolved" ]]; then
        printf 'Static runtime SDK input unavailable: %s\n' "$input" >&2
        exit 1
    fi
    install -m 0644 "$resolved" "$sdk/$input"
done

math_archive=$("$sdk_cxx" -print-file-name=libm.a)
[[ "$math_archive" == /* ]]
test -f "$math_archive"
if /usr/bin/ar t "$math_archive" >/dev/null 2>&1; then
    install -m 0644 "$math_archive" "$sdk/libm.a"
else
    # Some glibc toolchains provide libm.a as a script naming archives
    # by absolute host paths. Stage one archive with no host references.
    read -r -a math_parts <<<"$(awk '$1 == "GROUP" && $2 == "(" && $NF == ")" {
        for (i = 3; i < NF; ++i) printf "%s ", $i
    }' "$math_archive")"
    test "${#math_parts[@]}" -gt 0
    {
        printf 'CREATE libm.a\n'
        for part in "${math_parts[@]}"; do
            [[ "$part" == /*.a ]]
            /usr/bin/ar t "$part" >/dev/null
            printf 'ADDLIB %s\n' "$part"
        done
        printf 'SAVE\nEND\n'
    } | (cd "$sdk" && /usr/bin/ar -M)
fi

"$sdk_tool" manifest --prefix "$sdk" --manifest "$sdk/link-sdk.json" \
    --roots pipelang-objectc libpipelang_native_app_runtime.a "${inputs[@]}" libm.a
sha256sum "$sdk/link-sdk.json" | cut -d ' ' -f 1 > "$sdk/link-sdk.sha256"
