// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

class Recorder final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.PolymorphicFactory.Recorder";
    }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature == "ChooseDerived()" && arguments.empty())
            return Outcome::Success(derived);
        if (signature != "Record(int)" || arguments.size() != 1)
            return Outcome::Failure("unexpected cleanup recorder method");
        recorded = std::get<std::int64_t>(arguments.front());
        return Outcome::Success(recorded);
    }

    std::int64_t recorded = 0;
    bool derived = true;
};

template<class Outcome>
auto take(Outcome result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

int main()
{
    try {
        const auto recorder = std::make_shared<Recorder>();
        const std::array<native::HostedComposition::Input, 1> inputs{{{recorder->interfaceName(), recorder}}};
        const auto root = take(native::HostedComposition::bind(proof_root, {}, inputs));
        const auto base = take(root.resolve("NativeProof.PolymorphicFactory.Base"));
        const auto result = take(take(root.tryStartServiceMethod(base, "Read()", {})).result());
        if (result.integer() != 42)
            throw std::runtime_error("factory subtype did not retain virtual method dispatch");
        if (!root.close().is_success() || recorder->recorded != 42)
            throw std::runtime_error("factory subtype did not retain virtual cleanup dispatch");
        const auto exactPort = std::make_shared<Recorder>();
        exactPort->derived = false;
        const std::array<native::HostedComposition::Input, 1> exactInputs{{{exactPort->interfaceName(), exactPort}}};
        const auto exactRoot = take(native::HostedComposition::bind(proof_root, {}, exactInputs));
        const auto exact = take(exactRoot.resolve("NativeProof.PolymorphicFactory.Base"));
        const auto exactRead = take(take(exactRoot.tryStartServiceMethod(exact, "Read()", {})).result());
        if (exactRead.integer() != 10 || !exactRoot.close().is_success() || exactPort->recorded != 10)
            throw std::runtime_error("variant acceptance changed the original base factory result");

        const auto abortedPort = std::make_shared<Recorder>();
        const std::array<native::HostedComposition::Input, 1> abortedInputs{{{abortedPort->interfaceName(), abortedPort}}};
        const auto abortedRoot = take(native::HostedComposition::bind(proof_root, {}, abortedInputs));
        const auto scope = take(abortedRoot.createScope({}));
        const auto failure = scope.resolve("NativeProof.PolymorphicFactory.Failing");
        if (!failure.is_failure() || failure.error() != "polymorphic construction failed" ||
            abortedPort->recorded != 42 || take(scope.cleanupStatus()).attempted != 0 ||
            take(abortedRoot.cleanupStatus()).succeeded != 1)
            throw std::runtime_error("unpublished factory rollback lost its subtype or effective cleanup owner");
        if (!abortedRoot.close().is_success() || take(abortedRoot.cleanupStatus()).succeeded != 1)
            throw std::runtime_error("closing the root replayed the unpublished subtype cleanup");

        std::cout << "The checked factory kept the derived receiver and its method and cleanup overrides.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
