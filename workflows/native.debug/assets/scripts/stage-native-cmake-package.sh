#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

build=$1
prefix=$2
bindir=$(sed -n 's/^CMAKE_INSTALL_BINDIR:[^=]*=//p' "$build/CMakeCache.txt")
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' "$build/CMakeCache.txt")
datadir=$(sed -n 's/^CMAKE_INSTALL_DATADIR:[^=]*=//p' "$build/CMakeCache.txt")
if [[ -z "$datadir" ]]; then
    datadir=$(sed -n 's/^CMAKE_INSTALL_DATAROOTDIR:[^=]*=//p' "$build/CMakeCache.txt")
fi
for path in "$bindir" "$libdir" "$datadir"; do
    if [[ -z "$path" || "$path" == /* || "/$path/" == */../* ]]; then
        printf '%s\n' 'The CMake fixture requires relative SDK installation directories.' >&2
        exit 1
    fi
done
package="$prefix/$libdir/cmake/PipeLangModel"

/usr/bin/cmake --install "$build" --prefix "$prefix" --component PipeLangNativeExecutable
install -m 0644 "$build/PipeLangModelConfig.cmake" "$build/PipeLangModelConfigVersion.cmake" "$package/"
install -D -m 0755 "$build/sdk-tools/pipelang-sdk-tool" "$prefix/$bindir/pipelang-sdk-tool"
object_compiler="$datadir/pipelang-model/native-application/pipelang-objectc"
install -D -m 0755 "$build/sdk-tools/pipelang-objectc" "$prefix/$object_compiler"
printf '%s\n' "$package" > "$prefix/package-directory"
printf '%s\n' "$bindir" > "$prefix/bin-directory"
printf '%s\n' "$object_compiler" > "$prefix/object-compiler-path"
