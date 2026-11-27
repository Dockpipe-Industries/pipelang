#!/bin/bash
set -euo pipefail
: "${PIPELANG_CONTEXT_QT_PROOF:?Select fresh absolute proof path}"
: "${PIPELANG_CONTEXT_NATIVE_PROOF:?Select completed native candidate proof}"
: "${PIPELANG_CONTEXT_QT_ROOT:?Select explicit Qt host kit}"
proof="$PIPELANG_CONTEXT_QT_PROOF"
native="$PIPELANG_CONTEXT_NATIVE_PROOF"
sdk="$native/sdk"
runner="$sdk/bin/pipelang-contained"
[[ "$proof" == /* && "$proof/" != "$PWD/"* ]]
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]
if [[ "${1:-}" != --inside ]]; then
    /usr/bin/cmake -DFOUNDATION_NATIVE_JOB="$native/job.json" -P workflows/foundation.check/assets/wasm/require-native.cmake
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 900 -- \
        /usr/bin/env PIPELANG_CONTEXT_QT_PROOF="$proof" PIPELANG_CONTEXT_NATIVE_PROOF="$native" \
        PIPELANG_CONTEXT_QT_ROOT="$PIPELANG_CONTEXT_QT_ROOT" /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Aggregate containment required}"
mkdir "$proof/units" "$proof/source" "$proof/generated"
cp -a "$native/source/tests/native-models/qt-context/." "$proof/source/"
cp workflows/context.check/assets/qt/generate.cmake "$proof/source/"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/inputs.sha256"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 600 -- "$@" > "$proof/units/$name.launcher" 2>&1 || {
        tail -c 6000 "$proof/units/$name.output"
        return 1
    }
}
sdk_hash=$(cat "$native/sdk.sha256")
unit verify "$sdk/bin/pipelang-sdk-tool" verify --prefix "$sdk" \
    --manifest "$sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$sdk_hash"
unit generate /usr/bin/cmake "-DCONTEXT_SOURCE=$proof/source/ContextState.pipe" \
    "-DCONTEXT_COMPILER=$sdk/bin/pipelang-modelc" "-DCONTEXT_OUTPUT=$proof/generated" -P "$proof/source/generate.cmake"
binding_hash=$(sha256sum "$sdk/bin/pipelang-qt-bindc" | cut -d' ' -f1)
unit qt-generate /usr/bin/cmake "-DBINDING_COMPILER=$sdk/bin/pipelang-qt-bindc" \
    "-DBINDING_COMPILER_SHA256=$binding_hash" "-DMODEL_MANIFEST=$proof/generated/model.json" \
    -DSOURCE_MODEL=ContextState -DADAPTER_NAMESPACE=qt_context_binding -DADAPTER=ContextStateQt \
    "-DOUTPUT_HEADER=$proof/generated/ContextStateQt.hpp" "-DOUTPUT_RECEIPT=$proof/generated/qt-bindings.json" \
    -P "$sdk/lib/cmake/PipeLangModel/RunQtBindings.cmake"
for mode in normal sanitizer; do
    flags='-O1 -g1'
    if [[ "$mode" == sanitizer ]]; then
        flags+=' -fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    unit "$mode-configure" /usr/bin/cmake -S "$proof/source" -B "$proof/$mode" \
        "-DCMAKE_PREFIX_PATH=$PIPELANG_CONTEXT_QT_ROOT" "-DCMAKE_CXX_FLAGS=$flags" \
        "-DPIPELANG_QT_CONTEXT_SDK_PREFIX=$sdk" "-DPIPELANG_QT_CONTEXT_GENERATED_DIRECTORY=$proof/generated"
    unit "$mode-build" /usr/bin/cmake --build "$proof/$mode" --parallel 1
    unit "$mode-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$mode/qt-context-runtime"
done
sha256sum --check "$proof/inputs.sha256" > "$proof/inputs-verified.txt"

printf '%s\n' '{"protocol":"context-qualification.v1","status":"passed"}' > "$proof/complete.json"
