#!/bin/bash
set -euo pipefail
: "${PIPELANG_FOUNDATION_PROOF:?Select a fresh absolute proof directory outside the checkout}"
: "${PIPELANG_FOUNDATION_BASE_SDK:?Select an immutable native SDK}"
: "${PIPELANG_FOUNDATION_BASE_SHA256:?Pin the base SDK manifest}"
case "$PIPELANG_FOUNDATION_PROOF" in
    /*) ;;
    *) echo 'Foundation proof must be absolute' >&2; exit 1 ;;
esac
case "$PIPELANG_FOUNDATION_PROOF/" in
    "$PWD/"*) echo 'Foundation proof must be outside sources' >&2; exit 1 ;;
esac
runner="$PIPELANG_FOUNDATION_BASE_SDK/bin/pipelang-contained"
runner_hash=$(sha256sum "$runner")
if [[ "${runner_hash%% *}" != 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]; then
    echo 'Foundation runner identity differs from the qualified native runner' >&2
    exit 1
fi
proof="$PIPELANG_FOUNDATION_PROOF"
if [[ "${1:-}" != --inside ]]; then
    mkdir "$proof"
    exec "$runner" job --output "$proof/job" --estate "$proof" --timeout 1800 --         /usr/bin/env PIPELANG_FOUNDATION_PROOF="$proof"         PIPELANG_FOUNDATION_BASE_SDK="$PIPELANG_FOUNDATION_BASE_SDK"         PIPELANG_FOUNDATION_BASE_SHA256="$PIPELANG_FOUNDATION_BASE_SHA256"         /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Foundation verification requires aggregate containment}"
mkdir "$proof/source" "$proof/units"
git ls-files --cached --others --exclude-standard -z | tar --null -T - -cf "$proof/source.tar"
tar -xf "$proof/source.tar" -C "$proof/source"
find "$proof/source" -type f -print0 | sort -z | xargs -0 sha256sum > "$proof/inputs.sha256"
unit() {
    local name="$1"
    shift
    "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 900 -- "$@" > "$proof/units/$name.launcher" 2>&1 || {
        tail -c 6000 "$proof/units/$name.output"
        return 1
    }
}
unit base-verify "$PIPELANG_FOUNDATION_BASE_SDK/bin/pipelang-sdk-tool" verify \
    --prefix "$PIPELANG_FOUNDATION_BASE_SDK" \
    --manifest "$PIPELANG_FOUNDATION_BASE_SDK/share/pipelang-model/sdk-manifest.json" \
    --sha256 "$PIPELANG_FOUNDATION_BASE_SHA256"
unit configure /usr/bin/cmake -S "$proof/source/sdk/pipeview/model" -B "$proof/build" \
    -DCMAKE_BUILD_TYPE=Release \
    '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG -fno-inline --param=ggc-min-expand=10 --param=ggc-min-heapsize=4096' \
    -DPIPELANG_BUILD_SDK_TOOL_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
unit build /usr/bin/cmake --build "$proof/build" --parallel 1 --target \
    pipelang-modelc pipelang-model-viewc pipelang-sdk-tool pipelang-native-cpp pipelang-extended-tests pipelang-model-compat-tests pipelang-composition-factory-profile-tests
# This candidate replaces the model compiler and all SDK tools that link it.
# Other unchanged tools come from the admitted SDK. Record this composition in proof.
cp -a "$PIPELANG_FOUNDATION_BASE_SDK" "$proof/sdk"
cp "$proof/build/model-compiler/pipelang-modelc" "$proof/sdk/bin/"
cp "$proof/build/model-view-compiler/pipelang-model-viewc" "$proof/sdk/bin/"
cp "$proof/build/sdk-tools/pipelang-sdk-tool" "$proof/sdk/bin/"
cp "$proof/build/scalar-compiler/pipelang-native-cpp" "$proof/sdk/bin/"
cp -a "$proof/source/sdk/pipeview/model/native/include/." "$proof/sdk/include/"
unit manifest "$proof/sdk/bin/pipelang-sdk-tool" manifest --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --roots \
    bin/pipelang-modelc bin/pipelang-model-viewc bin/pipelang-sdk-tool bin/pipelang-contained \
    bin/pipelang-native bin/pipelang bin/pipelang-cpp-lexer bin/pipelang-cpp-view-parser \
    bin/pipelang-cpp-emitter bin/pipelang-native-cpp include/pipelang lib/cmake/PipeLangModel \
    share/pipelang-model lib/cmake/PipeLangNativeDependencies share/pipelang-native-dependencies
sdk_hash=$(sha256sum "$proof/sdk/share/pipelang-model/sdk-manifest.json")
sdk_hash=${sdk_hash%% *}
printf '%s\n' "$sdk_hash" > "$proof/sdk.sha256"
unit candidate-verify "$proof/sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$sdk_hash"
unit extended "$proof/build/sdk-tools/pipelang-extended-tests" "$proof/source" "$proof/sdk" "$proof/extended"
unit composition-profile "$proof/build/sdk-tools/pipelang-composition-factory-profile-tests"
for suite in iteration failure-cleanup; do
    unit "$suite-generate" "$proof/build/sdk-tools/pipelang-model-compat-tests" --suite "$suite" \
        --compiler "$proof/sdk/bin/pipelang-modelc" --source-root "$proof/source" --output "$proof/$suite"
done
for fixture in result_interface result_propagation result_propagation_worker typed_host_errors byte_buffer_iteration byte_buffers byte_buffer_objects; do
    for profile in normal sanitizer; do
        flags=(-O2)
        if [[ "$profile" == sanitizer ]]; then
            flags=(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        unit "$fixture-$profile-build" /usr/bin/c++ -std=c++20 -pthread -Wall -Wextra -Werror \
            "${flags[@]}" -I"$proof/sdk/include" -I"$proof/extended" \
            "$proof/source/tests/native-models/$fixture.cpp" -o "$proof/$fixture-$profile"
        unit "$fixture-$profile-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$fixture-$profile"
    done
done
for pair in 'iteration iteration-tests' 'failure-cleanup failure-cleanup'; do
    read -r suite file <<< "$pair"
    for profile in normal sanitizer; do
        flags=(-O2)
        if [[ "$profile" == sanitizer ]]; then
            flags=(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        unit "$suite-$profile-build" /usr/bin/c++ -std=c++20 -pthread -Wall -Wextra -Werror \
            "${flags[@]}" -I"$proof/sdk/include" -I"$proof/$suite" \
            "$proof/source/tests/native-models/$file.cpp" -o "$proof/$suite-$profile"
        unit "$suite-$profile-run" /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$suite-$profile"
    done
done
sha256sum --check "$proof/inputs.sha256" > "$proof/inputs-verified.txt"
unit candidate-verify-after "$proof/sdk/bin/pipelang-sdk-tool" verify --prefix "$proof/sdk" \
    --manifest "$proof/sdk/share/pipelang-model/sdk-manifest.json" --sha256 "$sdk_hash"
