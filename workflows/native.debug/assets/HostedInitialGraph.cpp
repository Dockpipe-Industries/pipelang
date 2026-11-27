// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifndef PIPELANG_GRAPH_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_initial_graph;
#endif

int main(int argc, char** argv)
{
    try {
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
        reference::source::NativeProof::HostedInitialGraph::Session bound;
        const auto invoke = [&](std::int64_t scenario) { return bound.Run(scenario); };
#else
        const auto bound = HostedInstance::bind(proof_hosted_initial_graph, {});
        if (!bound.is_success())
            throw std::runtime_error(bound.error());

        pipelang::model::TaskScope scope;
        const auto invoke = [&](std::int64_t scenario) {
            const std::array<HostedArgument, 1> arguments{scenario};
            return bound.value().start(scope, arguments).result();
        };
#endif
        const auto run = [&](std::int64_t scenario, std::int64_t expected) {
            const auto result = invoke(scenario);
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value() != expected)
                throw std::runtime_error("initial receiver lifetime did not match the source graph");
        };

        run(0, 42);
        run(1, 42);
        for (std::size_t batch = 0; batch < 64; ++batch)
            run(64, 42);
        run(66, 4096);
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            run(0, 42);
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t iteration = 0; iteration < 800; ++iteration)
                run(0, 42);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() << '\n';
            return 0;
        }
        run(1, 42);
        run(66, 0);
        std::cout << "Dropping the initial receiver released its allocation budget.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
