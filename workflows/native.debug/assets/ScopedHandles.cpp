// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

namespace {
using Composition = native::HostedComposition;

template<class Value>
Value requireValue(pipelang::model::Result<Value> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

native::HostedValue call(const Composition& scope, const Composition::Service& service, std::string_view method,
                         std::span<const Composition::Argument> arguments = {})
{
    const auto task = requireValue(scope.tryStartServiceMethod(service, method, arguments));
    require(task.completed(), "the synchronous scoped method suspended");
    return requireValue(task.result());
}
}

int main()
{
    try {
        const auto root = requireValue(Composition::bind(proof_root, {}, {}));
        const auto first = requireValue(root.createScope({}));
        const auto second = requireValue(root.createScope({}));
        constexpr std::string_view component = "NativeProof.ScopedHandles.Component";
        const auto firstService = requireValue(first.resolve(component));
        const auto secondService = requireValue(second.resolve(component));

        require(call(first, firstService, "Measure()").integer() == 0 &&
                call(second, secondService, "Measure()").integer() == 0,
                "implicit synchronization fields did not start empty");
        const std::array<Composition::Argument, 1> argument{native::HostedCallArgument{std::int64_t{3}}};
        require(call(first, firstService, "Add(int)", argument).integer() == 15 &&
                call(first, firstService, "Measure()").integer() == 6 &&
                call(second, secondService, "Measure()").integer() == 0,
                "implicit shared fields lost their values or crossed scope boundaries");

        const auto firstTask = requireValue(first.tryStartServiceMethod(firstService, "Wait()", {}));
        const auto secondTask = requireValue(second.tryStartServiceMethod(secondService, "Wait()", {}));
        require(!firstTask.completed() && !secondTask.completed(), "a default semaphore did not suspend");
        (void)call(first, firstService, "Signal()");
        for (int attempt = 0; attempt < 16 && !firstTask.completed(); ++attempt)
            (void)requireValue(root.poll());
        require(firstTask.completed() && requireValue(firstTask.result()).integer() == 3 && !secondTask.completed(),
                "a default semaphore released the wrong scoped receiver");
        require(root.close().is_success() && secondTask.completed() && secondTask.result().is_cancelled(),
                "root close left a default semaphore wait running");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
