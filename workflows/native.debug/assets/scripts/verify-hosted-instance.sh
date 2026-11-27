#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
compiler_record=$2
llc=$3
sdk_include=$4
proof=$5
assets=$(cd "$(dirname "$0")/.." && pwd -P)
cxx=$(<"$compiler_record")
mkdir "$proof"
"$sdk/pipelang-objectc" --source "$assets/HostedInstance.pipe" --entry NativeProof.HostedInstance.Session.Run \
    --host-symbol proof_hosted_session --llc "$llc" --output "$proof/session.o"
"$sdk/pipelang-objectc" --source "$assets/HostedReport.pipe" --entry NativeProof.HostedReport.Session.Run \
    --host-symbol proof_hosted_report --llc "$llc" --output "$proof/report.o"
"$sdk/pipelang-objectc" --source "$assets/HostedClock.pipe" --entry NativeProof.HostedClock.Session.Run \
    --host-symbol proof_hosted_clock --llc "$llc" --output "$proof/clock.o"
"$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    "$assets/HostedInstance.cpp" "$proof/session.o" "$proof/report.o" "$proof/clock.o" \
    "$sdk/libpipelang_native_app_runtime.a" -o "$proof/hosted-instance"
env -i "$proof/hosted-instance"

"$sdk/pipelang-objectc" --source "$assets/HostedReceivers.pipe" --entry NativeProof.HostedReceivers.Session.Run \
    --host-symbol proof_hosted_receivers --llc "$llc" --output "$proof/receivers.o"
"$cxx" -std=c++20 -O2 -fPIE -pie -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include" \
    "$assets/HostedReceivers.cpp" "$proof/receivers.o" "$sdk/libpipelang_native_app_runtime.a" -o "$proof/hosted-receivers"
env -i "$proof/hosted-receivers"
