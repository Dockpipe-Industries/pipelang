// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;
extern "C" const native::HostedModel proof_empty_interfaces;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

native::HostedValue checked(const native::HostedValueResult& result)
{
    require(result.is_success(), result.is_success() ? "" : result.error());
    return result.value();
}
}

int main()
{
    try {
        auto binding = native::HostedInstance::bind(proof_empty_interfaces, {});
        require(binding.is_success(), binding.is_success() ? "" : binding.error());
        auto instance = std::move(binding).value();
        model::TaskScope scope;

        const auto emptySuite = instance.startMethod(scope, "EmptySuite", {});
        require(emptySuite.completed() && checked(emptySuite.result()).integer() == 0,
                "imported runner changed the result for an empty interface list");
        const auto emptyNullable = instance.startMethod(scope, "EmptyNullable", {});
        require(emptyNullable.completed() && checked(emptyNullable.result()).boolean(),
                "nullable empty interface required an implementation");

        const std::array<native::HostedCallArgument, 1> absent{native::HostedValue{}};
        for (const std::string_view name : {"InvalidCase", "InvalidPure", "InvalidChild", "InvalidLater"}) {
            const auto rejected = instance.startMethod(scope, name, absent);
            require(rejected.completed() && rejected.result().is_failure(), name);
        }
        require(checked(instance.snapshot()).field("marker").integer() == 0,
                "invalid interface argument reached an authored side effect");
        const auto required = instance.startMethod(scope, "Required", {});
        require(required.completed() && required.result().is_failure(),
                "required reference without an implementation produced a valid object");

        const auto running = instance.startMethod(scope, "Run", {});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!running.completed()) {
            require(std::chrono::steady_clock::now() < deadline, "async-only source case did not finish");
            scope.poll();
            std::this_thread::yield();
        }
        require(checked(running.result()).integer() == 17, "async-only source case changed its result");
        require(scope.pending() == 0, "source interface calls retained tasks");
        std::cout << "Native empty source interface checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
