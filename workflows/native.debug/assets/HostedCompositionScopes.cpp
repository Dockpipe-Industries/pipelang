// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

class Input final : public native::HostedService {
public:
    Input(std::string_view contract, std::string_view signature, std::int64_t value)
        : contract_(contract), signature_(signature), value_(value)
    {
    }

    std::string_view interfaceName() const noexcept override { return contract_; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature != signature_ || !arguments.empty())
            return Outcome::Failure("unexpected hosted input method");
        return Outcome::Success(value_);
    }

private:
    std::string_view contract_;
    std::string_view signature_;
    std::int64_t value_;
};

void require(bool valid, const char* diagnostic)
{
    if (!valid)
        throw std::runtime_error(diagnostic);
}

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
        constexpr std::string_view consumer = "NativeProof.ScopedPorts.Consumer";
        constexpr std::string_view dependent = "NativeProof.ScopedPorts.Dependent";
        constexpr std::string_view singleton = "NativeProof.ScopedPorts.RootValue";
        constexpr std::string_view port = "NativeProof.ScopedPorts.Port";
        constexpr std::string_view offset = "NativeProof.ScopedPorts.Offset";
        constexpr std::string_view stamp = "NativeProof.ScopedPorts.Stamp";

        const std::array<Composition::Input, 1> singletons{{{offset, std::make_shared<Input>(offset, "Read()", 5)}}};
        const auto root = value(Composition::bind(proof_root, {}, singletons));
        const auto makeScope = [&](std::int64_t identity) {
            const std::array<Composition::Input, 2> inputs{{
                {port, std::make_shared<Input>(port, "Identity()", identity)},
                {stamp, native::HostedValue::fromRecord(std::string(stamp), {{"delta", native::HostedValue::fromInteger(2)}})}
            }};
            return value(root.createScope(inputs));
        };
        const std::array<Composition::Input, 2> wrongInputs{{
            {port, std::make_shared<Input>(port, "Identity()", 1)},
            {stamp, native::HostedValue::fromText("wrong snapshot")}
        }};
        require(root.createScope(wrongInputs).is_failure(), "scope admitted an input with the wrong nominal type");
        auto movable = makeScope(77);
        const auto moved = std::move(movable);
        require(movable.resolve(consumer).is_failure() && moved.close().is_success(),
                "a moved scope did not preserve safe handle ownership");

        const auto first = makeScope(35);
        const auto sibling = makeScope(101);
        const auto rejecting = makeScope(-1);
        const auto failure = rejecting.resolve(dependent);
        require(failure.is_failure() && failure.error() == "scope port rejected by authored constructor",
                "native SDK lost the authored construction failure");
        require(rejecting.close().is_success(), "rejected scope did not close");

        const auto shared = value(root.resolve(singleton));
        require(value(first.resolve(singleton)).sameObject(shared) && value(sibling.resolve(singleton)).sameObject(shared),
                "the native SDK failed to reuse a singleton");
        const auto firstService = value(first.resolve(consumer));
        const auto siblingService = value(sibling.resolve(consumer));
        require(value(first.resolve(consumer)).sameObject(firstService) && !firstService.sameObject(siblingService),
                "the native SDK failed to isolate scoped receivers");
        const auto transient = value(first.resolve(dependent));
        require(!value(first.resolve(dependent)).sameObject(transient), "the native SDK cached a transient receiver");

        const std::array<Composition::Argument, 1> next{transient};
        const auto count = value(first.tryStartMethod("Next", next)).result();
        require(count.is_success() && count.value().integer() == 3, "scoped transients lost their shared dependency");
        const auto foreign = sibling.tryStartMethod("Next", next);
        require(foreign.is_failure() && foreign.error() == "native composition service belongs to another scope",
                "a sibling scope could use a foreign scoped service handle");

        const auto cancellation = native::HostedCallArgument{model::CancellationToken{}};
        const std::array<Composition::Argument, 2> firstWait{firstService, cancellation};
        const std::array<Composition::Argument, 2> siblingWait{siblingService, cancellation};
        const auto parkedFirst = value(first.tryStartMethod("ReadLater", firstWait));
        const auto parkedSibling = value(sibling.tryStartMethod("ReadLater", siblingWait));
        require(!parkedFirst.completed() && !parkedSibling.completed(), "the scoped calls did not suspend");
        require(first.close().is_success(), "the native SDK could not close the first scope");
        require(parkedFirst.completed() && parkedFirst.result().is_cancelled() && !parkedSibling.completed(),
                "closing one native scope did not isolate its cancellation");
        const auto closed = first.resolve(consumer);
        require(closed.is_failure() && closed.error() == "di.scope_closed", "a closed scope admitted new resolution");

        const auto released = value(sibling.tryStartMethod("Release", {}));
        require(released.completed() && released.result().is_success(), "sibling could not release its pending source task");
        for (int attempt = 0; attempt < 16 && !parkedSibling.completed(); ++attempt)
            (void)value(sibling.poll());
        require(parkedSibling.completed() && parkedSibling.result().is_success() &&
                parkedSibling.result().value().integer() == 108, "sibling resumed with the wrong native receiver");
        const auto parkedAtClose = value(sibling.tryStartMethod("ReadLater", siblingWait));
        require(root.close().is_success() && parkedAtClose.completed() && parkedAtClose.result().is_cancelled(),
                "root close did not settle remaining child work");

        std::cout << "The native composition SDK resolved and closed a checked graph across isolated task groups.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
