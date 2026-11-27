// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

template<class Outcome>
auto value(Outcome result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

int main()
{
    try {
        using Composition = native::HostedComposition;
        const auto root = value(Composition::bind(proof_root, {}, {}));
        const auto scope = value(root.createScope({}));
        const auto collector = value(scope.resolve("NativeProof.AliasedExports.Collector"));
        const auto collected = value(scope.tryStartServiceMethod(collector, "ReadBoth()", {}));
        if (value(collected.result()).integer() != 84)
            throw std::runtime_error("checked construction lost its concrete typed dependencies");

        const std::array<Composition::Argument, 1> refuse{native::HostedCallArgument{native::HostedValue::fromBoolean(true)}};
        const auto authored = value(value(scope.tryStartServiceMethod(collector, "ReadStatus(bool)", refuse)).result());
        if (authored.outcome() != pipelang::model::Outcome::Failure || authored.error().text() != "authored refusal")
            throw std::runtime_error("a returned failure lost its authored result");
        const auto fault = value(scope.tryStartServiceMethod(collector, "ReadFault()", {})).result();
        if (!fault.is_failure() || fault.error() != "invocation fault")
            throw std::runtime_error("a source fault did not use the invocation outcome");

        const auto base = value(scope.resolve("NativeProof.AliasedExports.BaseValue"));
        const auto interface = value(scope.resolve("NativeProof.AliasedExports.IValue"));
        for (const auto& receiver : {base, interface}) {
            auto read = value(scope.tryStartServiceMethod(receiver, "Read()", {}));
            if (value(read.result()).integer() != 42)
                throw std::runtime_error("native service alias selected the wrong concrete read method");
        }
        if (!scope.tryStartServiceMethod(base, "AfterWait(int)", {}).is_failure())
            throw std::runtime_error("a class alias admitted an implementation-only method");

        const std::array<Composition::Argument, 1> arguments{native::HostedCallArgument{std::int64_t{101}}};
        const auto task = value(scope.tryStartServiceMethod(interface, "AfterWait(int)", arguments));
        if (task.completed())
            throw std::runtime_error("the aliased async method did not suspend");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!task.completed() && std::chrono::steady_clock::now() < deadline) {
            (void)value(scope.poll());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!task.completed() || value(task.result()).integer() != 143 || !root.close().is_success())
            throw std::runtime_error("the aliased continuation lost its concrete receiver");
        std::cout << "Native class and source interface exports used their checked concrete methods.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
