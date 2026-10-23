#!/usr/bin/env sh
set -eu
export GOWORK=off
go test -p 1 ./src/lib/application ./sdk/materialize ./src/cmd/... ./tests/pipelangcompat
go test -p 1 ./src/lib/pipelang -run '^(TestParse|TestAnalyze|TestCompile|TestInvoke|TestDiagnostic|TestSemantic)' -skip 'Memory|Generated|GoBackend'
