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
extern "C" const HostedModel proof_hosted_interface_field;
#endif

int main()
{
    try {
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
        reference::source::NativeProof::HostedInterfaceField::Session instance;
        const auto run = [&](std::int64_t scenario) { return instance.Run(scenario); };
#else
        const auto instance = HostedInstance::bind(proof_hosted_interface_field, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope;
        const auto run = [&](std::int64_t scenario) {
            const std::array<HostedArgument, 1> arguments{scenario};
            return instance.value().start(scope, arguments).result();
        };
#endif
        const auto verify = [&](std::int64_t scenario, std::int64_t expected) {
            const auto result = run(scenario);
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value() != expected)
                throw std::runtime_error("interface dispatch used the wrong receiver");
        };
        verify(0, 20);
        verify(1, 42);
        verify(0, 42);
        std::cout << "Hosted interface fields dispatched after construction and assignment.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
