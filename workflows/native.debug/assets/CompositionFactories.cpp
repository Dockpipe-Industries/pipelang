// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

class Checkpoint final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.CompositionFactories.Checkpoint";
    }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature != "Reach()" || !arguments.empty())
            return Outcome::Failure("unexpected factory checkpoint method");
        ++calls;
        cancelled.cancel();
        return Outcome::Success(std::int64_t{1});
    }

    model::CancellationSource cancelled;
    std::size_t calls = 0;
};

template<class Outcome>
auto take(Outcome result)
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

int main()
{
    try {
        using Composition = native::HostedComposition;
        constexpr std::string_view stamp = "NativeProof.CompositionFactories.Stamp";
        constexpr std::string_view service = "NativeProof.CompositionFactories.IValue";
        const auto checkpoint = std::make_shared<Checkpoint>();
        const std::array<Composition::Input, 1> rootInputs{{
            {checkpoint->interfaceName(), checkpoint}
        }};
        const auto root = take(Composition::bind(proof_root, {}, rootInputs));
        const auto scopeFor = [&](std::int64_t number) {
            const std::array<Composition::Input, 1> inputs{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"value", native::HostedValue::fromInteger(number)}})}
            }};
            return take(root.createScope(inputs));
        };
        const auto successful = scopeFor(42);
        const auto value = take(successful.resolve(service));
        require(take(successful.resolve(service)).sameObject(value), "factory result was not cached");
        require(take(take(successful.tryStartServiceMethod(value, "Read()", {})).result()).integer() == 42,
                "factory receiver was not preserved");
        const auto interrupted = scopeFor(43);
        const auto token = checkpoint->cancelled.token();
        const auto late = interrupted.resolve(service, token);
        require(late.is_cancelled() && checkpoint->calls == 1, "factory published after host-revoked cancellation");
        require(interrupted.resolve(service, token).is_cancelled() && checkpoint->calls == 1,
                "pre-cancelled construction entered the host");
        const auto retried = take(interrupted.resolve(service));
        require(checkpoint->calls == 2 &&
                take(take(interrupted.tryStartServiceMethod(retried, "Read()", {})).result()).integer() == 43,
                "cancelled construction left a published instance or prevented retry");

        const auto failure = scopeFor(-1).resolve(service);
        require(failure.is_failure() && failure.error() == "factory rejected input", "wrong factory failure");
        const auto cancelled = scopeFor(-2).resolve(service);
        require(cancelled.is_cancelled() && cancelled.error() == "factory cancelled", "wrong factory cancellation");
        const auto reused = scopeFor(-3).resolve(service);
        require(reused.is_failure() && reused.error() == "native scoped factory must return a freshly allocated receiver",
                "native factory accepted a receiver from an earlier recipe");
        require(root.close().is_success(), "composition closure failed");

        std::cout << "Native checked factories published fresh services and rolled back failure, cancellation and reuse.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
