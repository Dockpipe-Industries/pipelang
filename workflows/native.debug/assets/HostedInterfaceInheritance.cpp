// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace host = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const host::HostedModel proof_interface_inheritance;
extern "C" const host::HostedModel proof_modular_interface_inheritance;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

host::HostedValue checked(const host::HostedValueResult& outcome)
{
    require(outcome.is_success(), outcome.is_success() ? "" : outcome.error());
    return outcome.value();
}
}

int main()
{
    try {
        auto bound = host::HostedInstance::bind(proof_interface_inheritance, {});
        require(bound.is_success(), bound.is_success() ? "" : bound.error());
        auto instance = std::move(bound).value();
        model::TaskScope scope;
        const auto call = [&](std::string_view name, std::span<const host::HostedCallArgument> arguments = {}) {
            auto task = instance.startMethod(scope, name, arguments);
            require(task.completed(), "synchronous method suspended");
            return task.result();
        };

        require(checked(call("Identities")).boolean(), "diamond conversions lost receiver identity");
        require(checked(call("Values")).integer() == 72, "diamond dispatch produced the wrong result");
        require(checked(call("ExactChoice")).integer() == 103, "concrete overload changed");
        require(checked(call("DerivedChoice")).integer() == 303, "derived overload changed");
        require(checked(call("BaseChoice")).integer() == 203, "base overload changed");
        require(checked(call("TransitiveArguments")).integer() == 6, "transitive dispatch changed");
        const std::array<host::HostedCallArgument, 1> seven{std::int64_t{7}};
        require(checked(call("MutateThroughBase", seven)).integer() == 7, "base mutation lost the alias");
        const auto failed = call("FailInheritedMutation");
        require(failed.is_failure() && failed.error() == "inherited mutation fault", "inherited failure changed");
        require(checked(call("Read")).integer() == 7, "inherited mutation was not rolled back");

        auto waiting = instance.startMethod(scope, "WaitThroughBase", {});
        require(!waiting.completed(), "inherited async call did not suspend");
        const std::array<host::HostedCallArgument, 1> nine{std::int64_t{9}};
        require(checked(call("MutateThroughBase", nine)).integer() == 9, "parked call held the owner");
        require(call("ReleaseSignal").is_success(), "signal could not release the parked call");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!waiting.completed()) {
            scope.poll();
            require(std::chrono::steady_clock::now() < deadline, "inherited async call did not resume");
            std::this_thread::yield();
        }
        require(checked(waiting.result()).integer() == 9, "inherited async call lost the shared receiver");
        require(checked(call("Identities")).boolean(), "post-resume receiver identity changed");

        auto modular = host::HostedInstance::bind(proof_modular_interface_inheritance, {});
        require(modular.is_success(), modular.is_success() ? "" : modular.error());
        auto imported = modular.value().startValue(scope, {});
        require(imported.completed() && checked(imported.result()).integer() == 31,
                "inherited interface dispatch across modules returned the wrong value");
        std::cout << "Hosted interface inheritance passed across modules, rollback and suspension.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
