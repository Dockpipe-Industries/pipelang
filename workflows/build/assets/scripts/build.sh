#!/usr/bin/env sh
set -eu
export GOWORK=off
mkdir -p bin
for command in pipelang pipelang-streamc pipelang-native-cpp; do
    go build -buildvcs=false -trimpath -o "bin/$command" "./src/cmd/$command"
done
# The native emitter remains an explicitly selected experimental build.
if [ "${PIPELANG_BUILD_CPP_EMITTER:-0}" = 1 ]; then
    cmake -S src/native/cpp-emitter -B bin/cpp-emitter-build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/bin"
    cmake --build bin/cpp-emitter-build --parallel 2
fi
if [ "${PIPELANG_BUILD_CPP_LEXER:-0}" = 1 ]; then
    cmake -S src/native/lexer -B bin/cpp-lexer-build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/bin"
    cmake --build bin/cpp-lexer-build --parallel 2
fi

# PipeView authoring tools are optional; headless builds remain unchanged.
if [ "${PIPELANG_BUILD_PIPEVIEW_EXAMPLE:-0}" = 1 ]; then
    go build -buildvcs=false -trimpath -o bin/pipelang-pipeview-demo ./examples/pipeview-forgepipe
    go build -buildvcs=false -trimpath -o bin/pipelang-viewc ./src/cmd/pipelang-viewc
fi

# Syntax parsing is a separately selected experimental component.
if [ "${PIPELANG_BUILD_CPP_VIEW_PARSER:-0}" = 1 ]; then
    cmake -S src/native/view-parser -B bin/cpp-view-parser-build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/bin"
    cmake --build bin/cpp-view-parser-build --parallel 2
fi

# Ordinary models and typed view bridges are a separate, native-only SDK.
if [ "${PIPELANG_BUILD_NATIVE_MODELS:-0}" = 1 ]; then
    cmake -S sdk/pipeview/model -B bin/model-sdk-build \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build bin/model-sdk-build --parallel 1
fi
