// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include "SelfEscapeCheck.hpp"

#include <array>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_self_escape;

int main()
{
    try {
        const auto instance = HostedInstance::create(proof_self_escape, {});
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        pipelang::model::TaskScope scope;
        return verifySelfEscape([&](std::int64_t scenario) {
            const std::array<HostedArgument, 1> arguments{scenario};
            return instance.value().start(scope, arguments);
        }, [&scope] { scope.poll(); });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
