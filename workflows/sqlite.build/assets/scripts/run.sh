#!/usr/bin/env bash
set -euo pipefail
: "${PIPELANG_SQLITE_MODEL_PREFIX:?Select the exact installed PipeLangModel SDK}"
: "${PIPELANG_SQLITE_MODEL_MANIFEST_SHA256:?Pin the installed model SDK manifest}"
: "${PIPELANG_SQLITE_AMALGAMATION_ARCHIVE:?Select the pinned SQLite 3.54.1 archive}"
: "${PIPELANG_SQLITE_PROOF:?Select a fresh absolute proof directory}"
runner="$PIPELANG_SQLITE_MODEL_PREFIX/bin/pipelang-contained"
profile="${PIPELANG_SQLITE_PROFILE:-qualification}"
[[ "$profile" == qualification || "$profile" == release ]]
[[ "$PIPELANG_SQLITE_PROOF" == /* ]]
[[ "$(sha256sum "$runner" | cut -d' ' -f1)" == 19d55b36acc5796f86b44d2ee6e965991663d844c8468d86d20fab33e2272173 ]]
[[ "$(sha256sum "$PIPELANG_SQLITE_AMALGAMATION_ARCHIVE" | cut -d' ' -f1)" == 0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9 ]]
"$PIPELANG_SQLITE_MODEL_PREFIX/bin/pipelang-sdk-tool" verify --prefix "$PIPELANG_SQLITE_MODEL_PREFIX" \
    --manifest "$PIPELANG_SQLITE_MODEL_PREFIX/share/pipelang-model/sdk-manifest.json" \
    --sha256 "$PIPELANG_SQLITE_MODEL_MANIFEST_SHA256"
if [[ "${1:-}" != --inside ]]; then
    mkdir "$PIPELANG_SQLITE_PROOF"
    exec "$runner" job --output "$PIPELANG_SQLITE_PROOF/job" --estate "$PIPELANG_SQLITE_PROOF" --timeout 1800 -- \
        /usr/bin/env PIPELANG_SQLITE_MODEL_PREFIX="$PIPELANG_SQLITE_MODEL_PREFIX" \
        PIPELANG_SQLITE_MODEL_MANIFEST_SHA256="$PIPELANG_SQLITE_MODEL_MANIFEST_SHA256" \
        PIPELANG_SQLITE_AMALGAMATION_ARCHIVE="$PIPELANG_SQLITE_AMALGAMATION_ARCHIVE" \
        PIPELANG_SQLITE_PROFILE="$profile" \
        PIPELANG_SQLITE_PROOF="$PIPELANG_SQLITE_PROOF" /bin/bash "$0" --inside
fi
: "${CONTAINED_JOB_SLICE:?Verified aggregate containment is required}"
proof="$PIPELANG_SQLITE_PROOF"
mkdir "$proof/units" "$proof/source" "$proof/consumer"
unzip -p "$PIPELANG_SQLITE_AMALGAMATION_ARCHIVE" sqlite-amalgamation-3540100/sqlite3.c > "$proof/source/sqlite3.c"
unzip -p "$PIPELANG_SQLITE_AMALGAMATION_ARCHIVE" sqlite-amalgamation-3540100/sqlite3.h > "$proof/source/sqlite3.h"

unit() {
    local name="$1"
    shift
    if [[ "$name" != sanitizer-run ]]; then
        set -- /usr/bin/strace -f -e trace=execve -s 4096 -o "$proof/units/$name.exec" "$@"
    fi
    if "$runner" unit --output "$proof/units/$name" --estate "$proof" --timeout 900 -- "$@" \
        > "$proof/units/$name.launcher.log" 2>&1; then
        printf '%s: passed\n' "$name"
    else
        tail -c 6000 "$proof/units/$name.output" >&2
        tail -c 3000 "$proof/units/$name.launcher.log" >&2
        return 1
    fi
}

modes=(normal sanitizer)
[[ "$profile" != release ]] || modes=(normal)
for mode in "${modes[@]}"; do
    sanitizers=OFF
    [[ "$mode" != sanitizer ]] || sanitizers=ON
    unit "$mode-configure" /usr/bin/cmake -S modules/sqlite -B "$proof/$mode" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_C_COMPILER=/usr/bin/cc \
        -DCMAKE_PREFIX_PATH="$PIPELANG_SQLITE_MODEL_PREFIX" \
        -DPIPELANG_SQLITE_MODEL_MANIFEST_SHA256="$PIPELANG_SQLITE_MODEL_MANIFEST_SHA256" \
        -DPIPELANG_SQLITE_SOURCE_DIRECTORY="$proof/source" -DPIPELANG_SQLITE_SANITIZERS="$sanitizers" \
        -DPIPELANG_SQLITE_OPTIMIZATION=O3
    unit "$mode-build" /usr/bin/cmake --build "$proof/$mode" --parallel 1
    if [[ "$mode" == sanitizer ]]; then
        unit sanitizer-run /usr/bin/env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$proof/$mode/sqlite-native-tests" "$proof/$mode-fixtures"
    else
        unit normal-run "$proof/$mode/sqlite-native-tests" "$proof/$mode-fixtures"
        unit install /usr/bin/cmake --install "$proof/normal" --prefix "$proof/install"
    fi
done

cp modules/sqlite/tests/InstalledConsumer.cpp "$proof/consumer/main.cpp"
cat > "$proof/consumer/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21.1)
project(InstalledSqliteConsumer LANGUAGES CXX)
find_package(PipeLangSqlite 0.1.0 EXACT CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE PipeLang::Sqlite)
CMAKE
unit consumer-configure /usr/bin/cmake -S "$proof/consumer" -B "$proof/consumer/build" \
    -DCMAKE_PREFIX_PATH="$proof/install;$PIPELANG_SQLITE_MODEL_PREFIX" -DCMAKE_CXX_COMPILER=/usr/bin/c++
unit consumer-build /usr/bin/cmake --build "$proof/consumer/build" --parallel 1
unit consumer-run "$proof/consumer/build/consumer" "$proof/installed-consumer.sqlite"
for trace in "$proof"/units/*.exec; do
    "$PIPELANG_SQLITE_MODEL_PREFIX/bin/pipelang-sdk-tool" audit-exec --trace "$trace" --output "$trace.audit.json"
done
"$PIPELANG_SQLITE_MODEL_PREFIX/bin/pipelang-sdk-tool" verify --prefix "$PIPELANG_SQLITE_MODEL_PREFIX" \
    --manifest "$PIPELANG_SQLITE_MODEL_PREFIX/share/pipelang-model/sdk-manifest.json" \
    --sha256 "$PIPELANG_SQLITE_MODEL_MANIFEST_SHA256"
sha256sum "$proof/source/sqlite3.c" "$proof/source/sqlite3.h" "$proof/install/lib/libpipelang_sqlite.a" > "$proof/install/SHA256SUMS"
jq -n --arg model "$PIPELANG_SQLITE_MODEL_MANIFEST_SHA256" --arg profile "$profile" \
    --arg library "$(sha256sum "$proof/install/lib/libpipelang_sqlite.a" | cut -d' ' -f1)" \
    '{protocol:"pipelang.sqlite-native.v1",version:"0.1.0",sqlite_version:"3.54.1",model_manifest_sha256:$model,
      build_profile:$profile,optimization:"O3 (sanitized C++ boundary uses O1)",
      sqlite_archive_sha256:"0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9",
      library_sha256:$library,features:["SQLITE_THREADSAFE=1","SQLITE_ENABLE_FTS5=1","SQLITE_OMIT_LOAD_EXTENSION=1"],
      evidence:(["normal-native-fixtures","installed-CMake-consumer","native-execution-audit"] +
        (if $profile == "qualification" then ["ASan/UBSan-C++-boundary"] else [] end)),
      limitation:"SQLite C amalgamation itself is not sanitizer-instrumented; Linux/POSIX selected proof only"}' \
    > "$proof/install/sqlite-receipt.json"
printf 'SQLite candidate: %s/install\n' "$proof"
