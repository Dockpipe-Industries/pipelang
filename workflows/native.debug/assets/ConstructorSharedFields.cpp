// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <iostream>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_constructor_fields;

int main()
{
    const auto binding = native::HostedInstance::bind(proof_constructor_fields, {});
    if (!binding.is_success()) {
        std::cerr << binding.error() << '\n';
        return 1;
    }

    model::TaskScope tasks;
    const auto work = binding.value().start(tasks, {});
    if (!work.completed() || !work.result().is_success()) {
        if (work.completed())
            std::cerr << work.result().error() << '\n';
        return 1;
    }

    std::cout << work.result().value() << '\n';
    return work.result().value() == 111 ? 0 : 1;
}
