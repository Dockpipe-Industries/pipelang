#!/bin/bash
set -euo pipefail
: "${PIPELANG_CONTEXT_WASM_PROOF:?Select a fresh absolute proof path}"
: "${PIPELANG_CONTEXT_NATIVE_PROOF:?Select the completed native foundation proof}"
: "${PIPELANG_CONTEXT_WASM_SDK:?Select the admitted WASM SDK}"
: "${PIPELANG_CONTEXT_WASM_SHA256:?Pin the WASM SDK manifest}"
: "${PIPELANG_CONTEXT_EMSDK:?Select the installed Emscripten toolchain}"
: "${PIPELANG_CONTEXT_NODE:?Select the absolute Node executable}"
proof="$PIPELANG_CONTEXT_WASM_PROOF"
native="$PIPELANG_CONTEXT_NATIVE_PROOF"
base="$PIPELANG_CONTEXT_WASM_SDK"
case "$proof" in
    /*) ;;
    *) echo 'Foundation WASM proof must be absolute' >&2; exit 1 ;;
esac
case "$proof/" in
    "$PWD/"*) echo 'Foundation proof must be outside sources' >&2; exit 1 ;;
esac
runner="$base/bin/pipelang-contained"
runner_hash=$(sha256sum "$runner")
if [[ "${runner_hash%% *}" != 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]; then
    echo 'Foundation WASM runner identity differs' >&2
    exit 1
fi
if [[ "${1:-}" != --inside ]]; then
    /usr/bin/cmake -DFOUNDATION_NATIVE_JOB="$native/job.json" \
        -P workflows/foundation.check/assets/wasm/require-native.cmake
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 900 -- \
        /usr/bin/env PIPELANG_CONTEXT_WASM_PROOF="$proof" PIPELANG_CONTEXT_NATIVE_PROOF="$native" \
        PIPELANG_CONTEXT_WASM_SDK="$base" PIPELANG_CONTEXT_WASM_SHA256="$PIPELANG_CONTEXT_WASM_SHA256" \
        PIPELANG_CONTEXT_EMSDK="$PIPELANG_CONTEXT_EMSDK" PIPELANG_CONTEXT_NODE="$PIPELANG_CONTEXT_NODE" \
        /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?WASM proof requires aggregate containment}"
mkdir "$proof/units" "$proof/source"
cp -a workflows/context.check/assets/wasm/. "$proof/source/"
cp "$native/source/tests/native-models/context_owner_source.cpp" "$proof/source/"
cp "$native/extended/ContextOwnerSource.hpp" "$proof/source/"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/inputs.sha256"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 600 -- "$@" > "$proof/units/$name.launcher" 2>&1 || {
        tail -c 6000 "$proof/units/$name.output"
        return 1
    }
}
unit base-verify "$base/bin/pipelang-sdk-tool" verify --prefix "$base" \
    --manifest "$base/share/pipelang-model/sdk-manifest.json" --sha256 "$PIPELANG_CONTEXT_WASM_SHA256"
# Keep the WASM target/toolchain configuration while using the exact compiler-linked
# tools and runtime headers qualified from the current native source snapshot.
cp -a "$base" "$proof/sdk"
for tool in pipelang-modelc pipelang-model-viewc pipelang-sdk-tool pipelang-native-cpp pipelang-qt-bindc; do
    cp "$native/sdk/bin/$tool" "$proof/sdk/bin/"
done
cp -a "$native/source/sdk/pipeview/model/native/include/." "$proof/sdk/include/"
cp "$native/sdk/lib/cmake/PipeLangModel/"{PipeLangQtBindings,RunQtBindings}.cmake "$proof/sdk/lib/cmake/PipeLangModel/"
unit manifest "$proof/sdk/bin/pipelang-sdk-tool" manifest --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --roots \
    bin/pipelang-modelc bin/pipelang-model-viewc bin/pipelang-sdk-tool bin/pipelang-contained \
    bin/pipelang-native bin/pipelang bin/pipelang-cpp-lexer bin/pipelang-cpp-view-parser \
    bin/pipelang-cpp-emitter bin/pipelang-native-cpp bin/pipelang-qt-bindc include/pipelang \
    lib/cmake/PipeLangModel share/pipelang-model
runtime_hash=$(sha256sum "$proof/sdk/share/pipelang-model/sdk-manifest.json")
runtime_hash=${runtime_hash%% *}
printf '%s\n' "$runtime_hash" > "$proof/sdk.sha256"
unit candidate-verify "$proof/sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$runtime_hash"
export EMSDK_QUIET=1
source "$PIPELANG_CONTEXT_EMSDK/emsdk_env.sh" >/dev/null
export EM_CACHE="$proof/em-cache"
# The toolchain owns the initial sysroot cache. Build only in this proof's copy.
cp -a "$PIPELANG_CONTEXT_EMSDK/upstream/emscripten/cache" "$EM_CACHE"
export PATH="$(dirname "$PIPELANG_CONTEXT_NODE"):$PATH"
unit configure /usr/bin/env "PATH=$PATH" "EM_CACHE=$EM_CACHE" EMCC_CORES=1 BINARYEN_CORES=1 \
    "$PIPELANG_CONTEXT_EMSDK/upstream/emscripten/emcmake" /usr/bin/cmake \
    -S "$proof/source" -B "$proof/build" -DCMAKE_BUILD_TYPE=Release \
    "-DPipeLangModel_DIR=$proof/sdk/lib/cmake/PipeLangModel" \
    "-DFOUNDATION_RUNTIME_SHA256=$runtime_hash" "-DFOUNDATION_GENERATED=$proof/source" \
    "-DFOUNDATION_FIXTURE=$proof/source/context_owner_source.cpp"
unit build /usr/bin/env "PATH=$PATH" "EM_CACHE=$EM_CACHE" EMCC_CORES=1 BINARYEN_CORES=1 \
    /usr/bin/cmake --build "$proof/build" --parallel 1
unit run "$PIPELANG_CONTEXT_NODE" "$proof/build/context-owner-wasm.js"
sha256sum --check "$proof/inputs.sha256" > "$proof/inputs-verified.txt"
unit candidate-verify-after "$proof/sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$runtime_hash"

printf '%s\n' '{"protocol":"context-qualification.v1","status":"passed"}' > "$proof/complete.json"
