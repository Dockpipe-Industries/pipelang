#!/bin/bash
set -euo pipefail
: "${PIPELANG_FOUNDATION_PROJECT_PROOF:?Select a fresh absolute project proof}"
: "${PIPELANG_FOUNDATION_NATIVE_PROOF:?Select completed native proof}"
: "${PIPELANG_FOUNDATION_WASM_PROOF:?Select completed WASM proof}"
: "${PIPELANG_FOUNDATION_EMSDK:?Select Emscripten}"
: "${PIPELANG_FOUNDATION_NODE:?Select Node}"
proof="$PIPELANG_FOUNDATION_PROJECT_PROOF"
native="$PIPELANG_FOUNDATION_NATIVE_PROOF"
wasm="$PIPELANG_FOUNDATION_WASM_PROOF"
case "$proof" in
    /*) ;;
    *) echo 'Foundation project proof must be absolute' >&2; exit 1 ;;
esac
case "$proof/" in
    "$PWD/"*) echo 'Project proof must be outside sources' >&2; exit 1 ;;
esac
runner="$native/sdk/bin/pipelang-contained"
runner_hash=$(sha256sum "$runner")
test "${runner_hash%% *}" = 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173
if [[ "${1:-}" != --inside ]]; then
    for source_proof in "$native" "$wasm"; do
        /usr/bin/cmake -DFOUNDATION_NATIVE_JOB="$source_proof/job.json" \
            -P workflows/foundation.check/assets/wasm/require-native.cmake
    done
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 900 -- \
        /usr/bin/env PIPELANG_FOUNDATION_PROJECT_PROOF="$proof" PIPELANG_FOUNDATION_NATIVE_PROOF="$native" \
        PIPELANG_FOUNDATION_WASM_PROOF="$wasm" PIPELANG_FOUNDATION_EMSDK="$PIPELANG_FOUNDATION_EMSDK" \
        PIPELANG_FOUNDATION_NODE="$PIPELANG_FOUNDATION_NODE" /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Project proof requires containment}"
mkdir "$proof/source" "$proof/units"
cp -a workflows/foundation.check/assets/interface-project/. "$proof/source/"
cp "$native/source/tests/native-models/ResultInterface.pipe" "$proof/source/"
cp "$native/source/tests/native-models/ResultInterfaceChecks.hpp" "$proof/source/"
cp "$native/source/tests/native-models/result_interface.cpp" "$proof/source/"
cp "$native/extended/ResultInterface.hpp" "$proof/source/"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/inputs.sha256"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 600 -- "$@" > "$proof/units/$name.launcher" 2>&1 || {
        tail -c 6000 "$proof/units/$name.output"
        return 1
    }
}
native_hash=$(cat "$native/sdk.sha256")
wasm_hash=$(cat "$wasm/sdk.sha256")
for profile in normal sanitizer; do
    mode=OFF
    if [[ "$profile" == sanitizer ]]; then mode=ON; fi
    unit "$profile-configure" /usr/bin/cmake -S "$proof/source" -B "$proof/$profile" \
        -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' \
        "-DCMAKE_PREFIX_PATH=$native/sdk" "-DFOUNDATION_SDK_SHA256=$native_hash" \
        "-DFOUNDATION_FIXTURE_SOURCE=$proof/source/ResultInterface.pipe" "-DFOUNDATION_SANITIZERS=$mode"
    unit "$profile-build" /usr/bin/cmake --build "$proof/$profile" --parallel 1
    unit "$profile-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$profile/result-interface-project"
done
export EMSDK_QUIET=1
source "$PIPELANG_FOUNDATION_EMSDK/emsdk_env.sh" >/dev/null
export EM_CACHE="$proof/em-cache"
cp -a "$PIPELANG_FOUNDATION_EMSDK/upstream/emscripten/cache" "$EM_CACHE"
export PATH="$(dirname "$PIPELANG_FOUNDATION_NODE"):$PATH"
unit wasm-configure /usr/bin/env "PATH=$PATH" "EM_CACHE=$EM_CACHE" EMCC_CORES=1 BINARYEN_CORES=1 \
    "$PIPELANG_FOUNDATION_EMSDK/upstream/emscripten/emcmake" /usr/bin/cmake -S "$proof/source" -B "$proof/wasm" \
    -DCMAKE_BUILD_TYPE=Release "-DPipeLangModel_DIR=$wasm/sdk/lib/cmake/PipeLangModel" \
    "-DFOUNDATION_SDK_SHA256=$wasm_hash" "-DFOUNDATION_FIXTURE_SOURCE=$proof/source/ResultInterface.pipe"
unit wasm-build /usr/bin/env "PATH=$PATH" "EM_CACHE=$EM_CACHE" EMCC_CORES=1 BINARYEN_CORES=1 \
    /usr/bin/cmake --build "$proof/wasm" --parallel 1
unit wasm-run "$PIPELANG_FOUNDATION_NODE" "$proof/wasm/result-interface-project.js"
unit wasm-direct-run "$PIPELANG_FOUNDATION_NODE" "$proof/wasm/result-interface-direct.js"
sha256sum --check "$proof/inputs.sha256" > "$proof/inputs-verified.txt"
