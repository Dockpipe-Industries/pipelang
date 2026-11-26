// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_text_fold;

namespace {
thread_local bool recording = false;
thread_local std::uint64_t allocationCount = 0;
}

extern "C" void* __real__Znwm(std::size_t);

extern "C" void* __wrap__Znwm(std::size_t bytes)
{
    if (recording)
        ++allocationCount;
    return __real__Znwm(bytes);
}

int main()
{
    const auto session = HostedInstance::bind(proof_text_fold, {});
    if (!session.is_success()) {
        std::cerr << session.error() << '\n';
        return 1;
    }
    pipelang::model::TaskOptions options;
    options.evaluationStepsPerSegment = 1048576;
    pipelang::model::TaskScope scope(options);
    constexpr std::int64_t count = 4096;
    constexpr std::int64_t calls = 80;
    constexpr auto markerBytes = sizeof("source report prefix constant non-ASCII suffix é") - 1;
    constexpr auto expected = count * static_cast<std::int64_t>(markerBytes);
    const std::array<HostedArgument, 1> arguments{count};

    const auto start = std::chrono::steady_clock::now();
    recording = true;
    for (std::int64_t index = 0; index < calls; ++index) {
        const auto result = session.value().start(scope, arguments).result();
        if (!result.is_success() || result.value() != expected) {
            recording = false;
            std::cerr << (result.is_failure() ? result.error() : "incorrect text fold result") << '\n';
            return 1;
        }
    }
    recording = false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << elapsed << '\t' << allocationCount << '\n';
}
