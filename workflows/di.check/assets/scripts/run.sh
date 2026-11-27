#!/usr/bin/env bash
set -euo pipefail
: "${PIPELANG_DI_SDK:?Select an absolute installed SDK prefix}"
: "${PIPELANG_DI_SDK_SHA256:?Pin the selected SDK manifest}"
: "${PIPELANG_DI_PROOF:?Select a fresh absolute proof directory}"
benchmark="${PIPELANG_DI_BENCHMARK:-0}"
[[ "$PIPELANG_DI_SDK" == /* && "$PIPELANG_DI_PROOF" == /* ]]
[[ "$benchmark" == 0 || "$benchmark" == 1 ]]
runner="$PIPELANG_DI_SDK/bin/pipelang-contained"
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]
"$PIPELANG_DI_SDK/bin/pipelang-sdk-tool" verify --prefix "$PIPELANG_DI_SDK" \
    --manifest "$PIPELANG_DI_SDK/share/pipelang-model/sdk-manifest.json" --sha256 "$PIPELANG_DI_SDK_SHA256"
if [[ "${1:-}" != --inside ]]; then
    mkdir "$PIPELANG_DI_PROOF"
    exec "$runner" job --output "$PIPELANG_DI_PROOF/job" --estate "$PIPELANG_DI_PROOF" --timeout 1800 -- \
        /usr/bin/env PIPELANG_DI_SDK="$PIPELANG_DI_SDK" PIPELANG_DI_SDK_SHA256="$PIPELANG_DI_SDK_SHA256" \
        PIPELANG_DI_PROOF="$PIPELANG_DI_PROOF" PIPELANG_DI_BENCHMARK="$benchmark" /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Verified aggregate containment required}"
proof="$PIPELANG_DI_PROOF"
cp -a tests/native-di "$proof/source"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/source.sha256"
unit() {
    local name="$1"
    shift
    if "$runner" unit --output "$proof/$name" --estate "$proof" --timeout 600 -- "$@"; then return; fi
    tail -c 6000 "$proof/$name.output" >&2
    exit 1
}
for profile in header compiled sanitizer; do
    compilation=COMPILED
    sanitizers=OFF
    [[ "$profile" != header ]] || compilation=HEADER_ONLY
    [[ "$profile" != sanitizer ]] || sanitizers=ON
    for suite in ordinary lifecycle; do
        source="$proof/source"
        [[ "$suite" != lifecycle ]] || source="$source/lifecycle"
        build="$proof/$suite-$profile"
        measure=OFF
        if [[ "$profile" == compiled && "$benchmark" == 1 ]]; then measure=ON; fi
        unit "$suite-$profile-configure" /usr/bin/cmake -S "$source" -B "$build" \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PIPELANG_DI_SDK" \
            -DDI_EXPECTED_SDK_MANIFEST_SHA256="$PIPELANG_DI_SDK_SHA256" \
            -DDI_MODEL_COMPILATION="$compilation" -DDI_ENABLE_SANITIZERS="$sanitizers" \
            -DDI_BUILD_BENCHMARK="$measure" -DDI_BUILD_LIFECYCLE_BENCHMARK="$measure"
        unit "$suite-$profile-build" /usr/bin/cmake --build "$build" --parallel 1
        unit "$suite-$profile-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 /usr/bin/ctest --test-dir "$build" --output-on-failure
        if [[ "$measure" == ON ]]; then
            executable=pipelang-di-benchmark
            [[ "$suite" != lifecycle ]] || executable=pipelang-lifecycle-benchmark
            unit "$suite-benchmark" "$build/$executable"
        fi
    done
done
sha256sum --check "$proof/source.sha256"
"$PIPELANG_DI_SDK/bin/pipelang-sdk-tool" verify --prefix "$PIPELANG_DI_SDK" \
    --manifest "$PIPELANG_DI_SDK/share/pipelang-model/sdk-manifest.json" --sha256 "$PIPELANG_DI_SDK_SHA256"
