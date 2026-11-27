// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "FloatingObservationCheck.hpp"

#include <pipelang/model/native_host.hpp>

#include <iostream>

using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_floating_observation;

int main()
{
    try {
        const auto session = HostedInstance::bind(proof_floating_observation, {});
        if (!session.is_success())
            throw std::runtime_error(session.error());
        pipelang::model::TaskScope scope;
        checkFloatingObservation([&] { return session.value().start(scope, {}).result(); });
        std::cout << "Native floating observation preserved rounding modes and exception flags.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
