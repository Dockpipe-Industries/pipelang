// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>
#include <iostream>
#include <stdexcept>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_reclamation;

void require(bool valid, std::string_view error)
{
    if (!valid)
        throw std::runtime_error(std::string(error));
}

int main()
{
    try {
        const auto bound = HostedInstance::bind(proof_reclamation, {});
        require(bound.is_success(), bound.is_success() ? "" : bound.error());
        const auto& model = bound.value();
        pipelang::model::TaskScope scope;
        require(model.startMethod(scope, "CreateChild", {}).result().is_success(), "cannot create child");
        auto child = model.startContextOwner(scope, "ChildLifetime").result().value();
        bool ran = false;
        bool refused = false;
        bool readCommitted = false;
        const auto subscription = child->observeRevocation([&] {
            ran = true;
            const auto snapshot = model.snapshot();
            readCommitted = snapshot.is_success() && snapshot.value().field("changes").integer() == 0;
            const auto nested = model.startMethod(scope, "Replace", {}).result();
            refused = nested.is_failure() && nested.error() == "native hosted model already has an active invocation";
        });
        child.reset();
        require(model.startMethod(scope, "DropChild", {}).result().is_success(), "cannot remove child");
        require(ran && refused && readCommitted, "reclamation callback must only read committed state");
        require(model.startMethod(scope, "Changes", {}).result().value().integer() == 0, "callback changed owner state");
        std::cout << "Receiver reclamation preserves committed snapshots and excludes reentrant writers.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
