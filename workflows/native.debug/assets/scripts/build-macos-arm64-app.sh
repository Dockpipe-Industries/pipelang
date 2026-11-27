#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk_source=$1
native_object=$2
output=$3
test "$(/usr/bin/uname -m)" = arm64
sdk=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)
sdk_version=$(/usr/bin/xcrun --sdk macosx --show-sdk-version)
output_directory=$(cd "$(dirname "$output")" && pwd -P)
build=$(mktemp -d "$output_directory/.native-app.XXXXXXXX")
app="$build/$(basename "$output")"
symbols="$app.dSYM"
trap 'rm -rf -- "$build"' EXIT

# SDK staging. The application object was emitted by PipeLang and LLVM.
# No C++ compiler participates in the final application link.
objects=()
for unit in native_app_runtime native_app_launcher native_host native_host_value \
    native_composition NativeAtomics NativeBytes NativeConcurrentDictionaries NativeConcurrentSequences \
    NativeDictionaries NativeFloating NativeParallel NativeReceiverStorage NativeSequences; do
    object="$build/$unit.o"
    /usr/bin/xcrun --sdk macosx clang++ -std=c++20 -O2 -mmacosx-version-min=11.0 \
        -I "$sdk_source/include" -c "$sdk_source/$unit.cpp" -o "$object"
    objects+=("$object")
done
/usr/bin/xcrun ar rcs "$build/runtime.a" "${objects[@]}"
/bin/cp -- "$native_object" "$build/source.o"
# The Mach-O debug map records the linker's object paths and modification
# times. Keep both stable, then give dsymutil the actual staging directory.
TZ=UTC /usr/bin/touch -t 200001010000.00 "$build/source.o" "$build/runtime.a"
/usr/bin/xcrun --sdk macosx ld -arch arm64 -platform_version macos 11.0 "$sdk_version" \
    -syslibroot "$sdk" -oso_prefix "$build/" -o "$app" "$build/source.o" "$build/runtime.a" -lc++ -lSystem
/usr/bin/xcrun dsymutil --oso-prepend-path "$build" "$app" --out "$symbols"
if [[ -e "$output.dSYM" ]]; then
    /bin/mv -- "$output.dSYM" "$build/previous.dSYM"
fi
/bin/mv -f -- "$app" "$output"
/bin/mv -- "$symbols" "$output.dSYM"
