#!/bin/bash
set -euo pipefail
: "${PIPELANG_CONTEXT_REFRESH_PROOF:?Select fresh absolute proof path}"
: "${PIPELANG_CONTEXT_NATIVE_PROOF:?Select completed native proof}"
PIPELANG_CONTEXT_REFRESH_RUNTIME="${PIPELANG_CONTEXT_REFRESH_RUNTIME:-0}"
[[ "$PIPELANG_CONTEXT_REFRESH_RUNTIME" == 0 || "$PIPELANG_CONTEXT_REFRESH_RUNTIME" == 1 ]]
proof="$PIPELANG_CONTEXT_REFRESH_PROOF"
base="$PIPELANG_CONTEXT_NATIVE_PROOF"
runner="$base/sdk/bin/pipelang-contained"
[[ "$proof" == /* && "$proof/" != "$PWD/"* ]]
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]
if [[ "${1:-}" != --inside ]]; then
    /usr/bin/cmake -DFOUNDATION_NATIVE_JOB="$base/job.json" -P workflows/foundation.check/assets/wasm/require-native.cmake
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 1200 -- \
        /usr/bin/env PIPELANG_CONTEXT_REFRESH_PROOF="$proof" PIPELANG_CONTEXT_NATIVE_PROOF="$base" PIPELANG_CONTEXT_REFRESH_RUNTIME="$PIPELANG_CONTEXT_REFRESH_RUNTIME" /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Aggregate containment required}"
mkdir "$proof/source" "$proof/units"
git ls-files --cached --others --exclude-standard -z | tar --null -T - -cf "$proof/source.tar"
tar -xf "$proof/source.tar" -C "$proof/source"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/inputs.sha256"
# Reuse only compiler-linked tools with byte-identical owning sources, includes
# and build settings. The Qt generator is rebuilt; changed runtime headers require
# the explicit runtime requalification branch below.
for area in model-compiler model-view-compiler sdk-tools support scalar-compiler; do
    diff -qr "$base/source/src/native/$area" "$proof/source/src/native/$area" > "$proof/$area-identical.txt"
done
if [[ "$PIPELANG_CONTEXT_REFRESH_RUNTIME" == 0 ]]; then
    for area in model di; do
        diff -qr "$base/source/sdk/pipeview/model/native/include/pipelang/$area" \
            "$proof/source/sdk/pipeview/model/native/include/pipelang/$area" > "$proof/$area-runtime-identical.txt"
    done
fi
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 600 -- "$@" > "$proof/units/$name.launcher" 2>&1 || {
        tail -c 6000 "$proof/units/$name.output"
        return 1
    }
}
base_hash=$(cat "$base/sdk.sha256")
unit base-verify "$base/sdk/bin/pipelang-sdk-tool" verify --prefix "$base/sdk" \
    --manifest "$base/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$base_hash"
unit qt-configure /usr/bin/cmake -S "$proof/source/src/native/qt-model-bindings" -B "$proof/qt-build" \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG'
unit qt-build /usr/bin/cmake --build "$proof/qt-build" --parallel 1
unit qt-metadata "$proof/qt-build/pipelang-qt-binding-tests"
cp -a "$base/sdk" "$proof/sdk"
cp "$proof/qt-build/pipelang-qt-bindc" "$proof/sdk/bin/"
cp -a "$proof/source/sdk/pipeview/model/native/include/." "$proof/sdk/include/"
cp "$proof/source/sdk/pipeview/model/cmake/"{PipeLangQtBindings,RunQtBindings}.cmake "$proof/sdk/lib/cmake/PipeLangModel/"
unit manifest "$proof/sdk/bin/pipelang-sdk-tool" manifest --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --roots \
    bin/pipelang-modelc bin/pipelang-model-viewc bin/pipelang-sdk-tool bin/pipelang-contained \
    bin/pipelang-native bin/pipelang bin/pipelang-cpp-lexer bin/pipelang-cpp-view-parser \
    bin/pipelang-cpp-emitter bin/pipelang-native-cpp bin/pipelang-qt-bindc include/pipelang lib/cmake/PipeLangModel \
    share/pipelang-model lib/cmake/PipeLangNativeDependencies share/pipelang-native-dependencies
sdk_hash=$(sha256sum "$proof/sdk/share/pipelang-model/sdk-manifest.json" | cut -d' ' -f1)
printf '%s\n' "$sdk_hash" > "$proof/sdk.sha256"
if [[ "$PIPELANG_CONTEXT_REFRESH_RUNTIME" == 0 ]]; then
    cp -a "$base/extended" "$proof/extended"
else
    unit extended "$base/build/sdk-tools/pipelang-extended-tests" "$proof/source" "$proof/sdk" "$proof/extended"
    unit source-profile "$base/build/sdk-tools/pipelang-context-source-profile-tests"
    for mode in normal sanitizer; do
        flags=(-O2)
        if [[ "$mode" == sanitizer ]]; then
            flags=(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for fixture in ContextOwnership ContextPublication; do
            unit "$fixture-$mode-build" /usr/bin/c++ -std=c++20 -pthread -Wall -Wextra -Werror \
                "${flags[@]}" -I"$proof/sdk/include" "$proof/source/tests/native-models/${fixture}Tests.cpp" \
                -o "$proof/$fixture-$mode"
            unit "$fixture-$mode-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$fixture-$mode"
        done
        for fixture in context_owner_source result_interface result_propagation result_propagation_worker typed_host_errors byte_buffer_iteration byte_buffers byte_buffer_objects; do
            unit "$fixture-$mode-build" /usr/bin/c++ -std=c++20 -pthread -Wall -Wextra -Werror \
                "${flags[@]}" -I"$proof/sdk/include" -I"$proof/extended" \
                "$proof/source/tests/native-models/$fixture.cpp" -o "$proof/$fixture-$mode"
            unit "$fixture-$mode-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$fixture-$mode"
        done
    done
    unit allocation-build /usr/bin/c++ -std=c++20 -pthread -O2 -Wall -Wextra -Werror \
        -I"$proof/sdk/include" "$proof/source/tests/native-di/OrdinaryExecutionAllocationTests.cpp" -o "$proof/allocation"
    unit allocation-run "$proof/allocation"
fi
unit candidate-verify "$proof/sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$sdk_hash"
sha256sum --check "$proof/inputs.sha256" > "$proof/inputs-verified.txt"

printf '%s\n' '{"protocol":"context-qualification.v1","status":"passed"}' > "$proof/complete.json"
