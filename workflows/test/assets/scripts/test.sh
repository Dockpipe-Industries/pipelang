#!/usr/bin/env sh
set -eu
export GOWORK=off
go test -p 1 ./src/lib/application ./sdk/materialize ./sdk/pipeview/... ./examples/pipeview-forgepipe ./src/cmd/... ./src/lib/pipelang/cppbackend ./src/lib/pipelang/internal/nativeprocess ./src/lib/pipelang/nativelexer ./src/lib/pipelang/nativeviewparser ./tests/pipelangcompat
go test -p 1 ./src/lib/pipelang -run '^(TestParse|TestAnalyze|TestCompile|TestInvoke|TestDiagnostic|TestSemantic|TestFormatView)' -skip 'Memory|Generated|GoBackend'

python3 -B tests/pipeview/check_packages.py

python3 -B tests/pipeview/check_test_handles.py
