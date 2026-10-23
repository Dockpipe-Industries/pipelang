#!/usr/bin/env sh
set -eu
export GOWORK=off
mkdir -p bin
for command in pipelang pipelang-streamc pipelang-native-cpp; do
    go build -buildvcs=false -trimpath -o "bin/$command" "./src/cmd/$command"
done
