# SPDX-License-Identifier: Apache-2.0
if(NOT ACTION STREQUAL "build")
    message(FATAL_ERROR "Go profile supports native archive builds")
endif()
pipelang_execute("GOTOOLCHAIN=local" "GOPROXY=off" "GOSUMDB=off" "GOENV=off" "GOWORK=off"
    "GOCACHE=${BUILD_DIRECTORY}/cache" "GOMODCACHE=${CACHE_DIRECTORY}" "CGO_ENABLED=1"
    "GOMAXPROCS=1" "GO111MODULE=on" "GOOS=linux" "GOARCH=amd64" "CC=${tool_cc}"
    "${tool_go}" build -p=1 -mod=readonly -trimpath -buildvcs=false -buildmode=c-archive
    -o "${RESOLVED_ARTIFACT}" .)
