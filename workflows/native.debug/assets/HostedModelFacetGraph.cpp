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
extern "C" const HostedModel proof_hosted_model_facet_graph;
#endif

int main()
{
    try {
#ifdef PIPELANG_GRAPH_CPP_REFERENCE
        reference::source::NativeProof::HostedModelFacetGraph::Session instance;
        const auto run = [&](std::int64_t scenario) { return instance.Run(scenario); };
#else
        const auto instance = HostedInstance::bind(proof_hosted_model_facet_graph, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope;
        const auto run = [&](std::int64_t scenario) {
            const std::array<HostedArgument, 1> arguments{scenario};
            return instance.value().start(scope, arguments).result();
        };
#endif
        if (run(0).value() != 40 || run(1).value() != 42)
            throw std::runtime_error("the source facet lost its object identity");
        const auto failed = run(2);
        if (!failed.is_failure() || failed.error() != "score changes rolled back")
            throw std::runtime_error("the source facet changed the rollback outcome");
        if (run(3).value() != 142)
            throw std::runtime_error("the source facet published failed collection or object writes");
        std::cout << "A hosted collection preserved the other model's source facet.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
