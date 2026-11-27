// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <iostream>
#include <stdexcept>

#ifndef PIPELANG_GRAPH_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_graph_cycles;
#endif

int main()
{
    try {
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
        reference::source::NativeProof::HostedGraphCycles::Session app;
        const auto run = [&](std::int64_t scenario) { return app.Run(scenario); };
#else
        const auto app = HostedInstance::bind(proof_hosted_graph_cycles, {});
        if (!app.is_success())
            throw std::runtime_error(app.error());
        pipelang::model::TaskScope scope;
        const auto run = [&](std::int64_t scenario) {
            const std::array<HostedArgument, 1> arguments{scenario};
            return app.value().start(scope, arguments).result();
        };
#endif
        const auto verify = [&](std::int64_t scenario) {
            const auto result = run(scenario);
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value() != 42)
                throw std::runtime_error("graph cycle lost identity");
        };
        verify(0);
        verify(1);
        for (std::size_t count = 0; count < 4200; ++count) {
            verify(3);
            verify(4);
            verify(5);
        }
        verify(1);
        verify(2);
        for (std::size_t count = 0; count < 4200; ++count)
            verify(0);
        verify(1);
        std::cout << "Receiver cycles, failed constructions and temporary locks retained their lifetimes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
