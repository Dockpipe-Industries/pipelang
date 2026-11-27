#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

runtime=$1
sdk_tool=$2
sdk=$3

mkdir "$sdk"
install -m 0644 "$runtime" "$sdk/libpipelang_native_app_runtime.a"
"$sdk_tool" manifest --prefix "$sdk" --manifest "$sdk/link-sdk.json" --roots libpipelang_native_app_runtime.a
shasum -a 256 "$sdk/link-sdk.json" | cut -d ' ' -f 1 > "$sdk/link-sdk.sha256"
