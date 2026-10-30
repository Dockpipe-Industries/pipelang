#!/usr/bin/env sh
set -eu
export GOWORK=off
go test -p 1 ./src/lib/application ./sdk/materialize ./sdk/pipeview/... ./examples/pipeview-forgepipe ./src/cmd/... ./src/lib/pipelang/cppbackend ./src/lib/pipelang/internal/nativeprocess ./src/lib/pipelang/nativelexer ./src/lib/pipelang/nativeviewparser ./tests/pipelangcompat
go test -p 1 ./src/lib/pipelang -run '^(TestParse|TestAnalyze|TestCompile|TestInvoke|TestDiagnostic|TestSemantic|TestFormatView)' -skip 'Memory|Generated|GoBackend'

python3 -B tests/pipeview/check_packages.py

python3 -B tests/pipeview/check_test_handles.py

# Build and qualify a fresh native SDK when explicitly selected.
if [ "${PIPELANG_TEST_NATIVE_MODELS:-0}" = 1 ]; then
    : "${PIPELANG_NATIVE_MODEL_PROOF:?Select a fresh proof directory outside the checkout}"
    python3 -B tests/native-models/qualify.py \
        --source-root "$PWD" \
        --build-sdk \
        --output "$PIPELANG_NATIVE_MODEL_PROOF"
fi
