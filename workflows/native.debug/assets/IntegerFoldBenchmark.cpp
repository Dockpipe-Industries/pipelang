// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifndef CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_integer_fold;
#endif

int main(int argc, char** argv)
{
    try {
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 1048576;
#ifdef CPP_REFERENCE
        reference::source::NativeProof::IntegerFold::Session app(options);
        const auto run = [&app](std::int64_t count) { return app.Run(count).value(); };
#else
        auto app = HostedInstance::bind(proof_integer_fold, {});
        if (!app.is_success())
            throw std::runtime_error(app.error());
        pipelang::model::TaskScope scope(options);
        const auto run = [&](std::int64_t count) {
            const std::array<HostedArgument, 1> arguments{count};
            const auto result = app.value().start(scope, arguments).result();
            if (!result.is_success())
                throw std::runtime_error(result.error());
            return result.value();
        };
#endif
        if (run(-1) != 41 || run(-2) != 42 || run(-3) != 0)
            throw std::runtime_error("checked integer fault or remainder changed");

        const auto start = std::chrono::steady_clock::now();
        for (std::size_t invocation = 0; invocation < 80; ++invocation) {
            if (run(4096) != 4096 * 42)
                throw std::runtime_error("checked constant arithmetic returned the wrong result");
        }
        if (argc == 2 && std::string_view(argv[1]) == "--measure") {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << elapsed << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
