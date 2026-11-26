#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

object_compiler=$1
toolchain=$2
source_root=$3
sdk=$4
sdk_tool=$5
architecture=${6:-x86_64}
case "$architecture" in
    x86_64|aarch64)
        ;;
    *)
        printf 'Unsupported Windows SDK architecture %s\n' "$architecture" >&2
        exit 1
        ;;
esac
cxx="$toolchain/bin/$architecture-w64-mingw32-clang++"

mkdir "$sdk" "$sdk/link"
install -m 0755 "$object_compiler" "$sdk/pipelang-objectc"
runtime_build=$(mktemp -d "${sdk}.runtime-build.XXXXXXXX")
trap 'cmake -E rm -rf "$runtime_build"' EXIT
cmake -S "$source_root/sdk/pipeview/model" -B "$runtime_build" \
    -DCMAKE_SYSTEM_NAME=Windows "-DCMAKE_SYSTEM_PROCESSOR=$architecture" \
    "-DCMAKE_CXX_COMPILER=$cxx" \
    "-DCMAKE_RC_COMPILER=$toolchain/bin/$architecture-w64-mingw32-windres" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-O2 \
    -DPIPELANG_BUILD_NATIVE_OBJECT_PROOF=ON
cmake --build "$runtime_build" --parallel 1 --target pipelang_native_app_runtime
install -m 0644 "$runtime_build/libpipelang_native_app_runtime.a" "$sdk/link/libpipelang_native_app_runtime.a"

inputs=(crt2.o crtbegin.o crtend.o libc++.a libmingw32.a libunwind.a libmoldname.a
    libmingwex.a libmsvcrt.a libpthread.a libadvapi32.a libshell32.a libuser32.a libkernel32.a)
for input in "${inputs[@]}"; do
    resolved=$("$cxx" "-print-file-name=$input")
    if [[ "$resolved" != /* || ! -f "$resolved" ]]; then
        printf 'Windows runtime SDK input unavailable: %s\n' "$input" >&2
        exit 1
    fi
    install -m 0644 "$resolved" "$sdk/link/$input"
done
resource=$("$cxx" -print-resource-dir)
builtins="libclang_rt.builtins-$architecture.a"
install -m 0644 "$resource/lib/windows/$builtins" "$sdk/link/$builtins"

"$sdk_tool" manifest --prefix "$sdk" --manifest "$sdk/link-sdk.json" --roots pipelang-objectc link
sha256sum "$sdk/link-sdk.json" | cut -d ' ' -f 1 > "$sdk/link-sdk.sha256"
