// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

class Port final : public native::HostedService {
public:
    explicit Port(std::int64_t identity) : identity_(identity) {}

    std::string_view interfaceName() const noexcept override { return "NativeProof.ScopedPorts.Port"; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature == "Pause(CancellationToken)" && arguments.size() == 1) {
            const auto* token = std::get_if<model::CancellationToken>(&arguments.front());
            if (!token)
                return Outcome::Failure("borrowed port had no cancellation token");
            entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!token->is_cancelled() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!token->is_cancelled())
                return Outcome::Failure("borrowed port was not cancelled before deadline");
            return Outcome::Cancelled("borrowed port cancelled");
        }
        if (signature != "Identity()" || !arguments.empty())
            return Outcome::Failure("unexpected borrowed port method");
        ++calls;
        if (revokeOnIdentity)
            revokeOnIdentity->invalidate();
        return Outcome::Success(identity_);
    }

    model::ContextOwner* revokeOnIdentity = nullptr;
    std::atomic_bool entered{false};
    std::size_t calls = 0;

private:
    std::int64_t identity_;
};

class Offset final : public native::HostedService {
public:
    explicit Offset(std::int64_t value) : value_(value) {}

    std::string_view interfaceName() const noexcept override { return "NativeProof.ScopedPorts.Offset"; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature != "Read()" || !arguments.empty())
            return Outcome::Failure("unexpected borrowed offset method");
        ++calls;
        return Outcome::Success(value_);
    }

    std::size_t calls = 0;

private:
    std::int64_t value_;
};

native::HostedValue stamp(std::int64_t delta)
{
    return native::HostedValue::fromRecord("NativeProof.ScopedPorts.Stamp", {
        {"delta", native::HostedValue::fromInteger(delta)}
    });
}

void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

native::HostedValueResult read(const native::HostedInstance& root, model::TaskScope& tasks,
                               const native::HostedValue& consumer)
{
    const std::array<native::HostedCallArgument, 1> argument{consumer};
    return root.startMethod(tasks, "Read", argument).result();
}

template <typename Predicate>
void waitUntil(model::TaskScope& tasks, Predicate predicate, const char* diagnostic)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        tasks.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(predicate(), diagnostic);
}

void settleScopedHostCall()
{
    constexpr std::string_view service = "NativeProof.ScopedPorts.Consumer";
    const auto root = native::HostedInstance::bind(proof_root, {});
    require(root.is_success(), "could not initialize scoped root");
    auto firstPort = std::make_shared<Port>(101);
    const std::weak_ptr<Port> firstLifetime = firstPort;
    const auto first = [&] {
        const std::array<native::HostedConstructorArgument, 3> inputs{
            firstPort, std::make_shared<Offset>(5), stamp(2)
        };
        return root.value().constructService(service, inputs);
    }();
    require(first.is_success(), "could not construct first scope receiver");

    model::TaskScope tasks;
    const auto firstGroup = tasks.createGroup();
    const auto siblingGroup = tasks.createGroup();
    const std::array<native::HostedCallArgument, 2> firstArguments{first.value(), model::CancellationToken{}};
    const auto inHost = root.value().tryStartMethod(tasks, firstGroup, "Hold", firstArguments);
    if (inHost.is_failure())
        throw std::runtime_error(inHost.error());
    require(inHost.is_success(), "first group did not admit host call");
    waitUntil(tasks, [&] { return firstPort->entered.load(std::memory_order_acquire); },
              "first scope did not enter borrowed host port");

    const std::array<native::HostedConstructorArgument, 3> siblingInputs{
        std::make_shared<Port>(202), std::make_shared<Offset>(7), stamp(3)
    };
    const auto sibling = root.value().constructService(service, siblingInputs);
    require(sibling.is_success(), "a live host call prevented construction of an independent sibling");
    const std::array<native::HostedCallArgument, 2> siblingArguments{sibling.value(), model::CancellationToken{}};
    const auto parked = root.value().tryStartMethod(tasks, siblingGroup, "ReadLater", siblingArguments);
    require(parked.is_success() && !parked.value().completed(), "sibling did not park on its source wait");

    const auto busy = root.value().retireService(first.value());
    require(busy.is_failure() && busy.error() == "native scoped port has an active host call",
            "retirement did not preserve a borrowed port in use");
    firstPort.reset();
    require(!firstLifetime.expired(), "active call lost its borrowed service");

    tasks.drainGroup(firstGroup);
    require(inHost.value().completed() && inHost.value().result().is_cancelled(),
            "group drain did not settle its borrowed host call as cancelled");
    const auto retired = root.value().retireService(first.value());
    if (retired.is_failure())
        throw std::runtime_error(retired.error());
    require(retired.is_success() && firstLifetime.expired(), "retiring the drained scope retained its service");
    require(!parked.value().completed() && tasks.pending(siblingGroup) == 1,
            "retiring the first scope changed a sibling task");

    const auto released = root.value().tryStartMethod(tasks, siblingGroup, "Release", {});
    require(released.is_success() && released.value().result().is_success(), "could not release sibling");
    waitUntil(tasks, [&] { return parked.value().completed(); }, "sibling task did not resume");
    require(parked.value().result().is_success() && parked.value().result().value().integer() == 212,
            "independent sibling did not complete with its own scoped inputs");
}

int main()
{
    try {
        constexpr std::string_view service = "NativeProof.ScopedPorts.Consumer";
        const auto root = native::HostedInstance::bind(proof_root, {});
        const auto foreign = native::HostedInstance::bind(proof_root, {});
        require(root.is_success() && foreign.is_success(), "could not build distinct graph owners");
        const auto snapshot = root.value().snapshot();
        require(snapshot.is_success() && snapshot.value().kind() == native::HostedValue::Kind::Record,
                "composition descriptor did not preserve the hosted snapshot");
        const auto missing = root.value().constructService("NativeProof.ScopedPorts.Missing", {});
        require(missing.is_failure() && missing.error() == "native hosted constructor recipe was not found",
                "undeclared service recipe did not fail directly");
        const auto incomplete = root.value().constructService(service, {});
        require(incomplete.is_failure() && incomplete.error() == "native hosted constructor argument count does not match",
                "missing checked constructor arguments did not fail directly");

        const auto firstPort = std::make_shared<Port>(101);
        const auto secondPort = std::make_shared<Port>(202);
        const auto firstOffset = std::make_shared<Offset>(5);
        const std::array<native::HostedConstructorArgument, 3> firstInputs{firstPort, firstOffset, stamp(2)};
        const auto first = root.value().constructService(service, firstInputs);
        if (!first.is_success())
            throw std::runtime_error(first.error());

        model::TaskScope tasks;
        const auto group = tasks.createGroup();
        const std::array<native::HostedCallArgument, 2> delayed{first.value(), model::CancellationToken{}};
        auto pending = root.value().tryStartMethod(tasks, group, "ReadLater", delayed);
        require(pending.is_success() && !pending.value().completed(), "first scoped task did not suspend");

        const auto secondOffset = std::make_shared<Offset>(7);
        const std::array<native::HostedConstructorArgument, 3> secondInputs{secondPort, secondOffset, stamp(3)};
        const auto second = root.value().constructService(service, secondInputs);
        if (second.is_failure())
            throw std::runtime_error(second.error());
        require(first.is_success() && second.is_success() && !first.value().sameObject(second.value()),
                "shared native graph did not admit distinct scoped receivers");

        auto refusedPort = std::make_shared<Port>(-1);
        std::weak_ptr<Port> rejectedLifetime = refusedPort;
        const auto rejected = [&] {
            const std::array<native::HostedConstructorArgument, 3> inputs{refusedPort, secondOffset, stamp(3)};
            return root.value().constructService(service, inputs);
        }();
        require(rejected.is_failure() && rejected.error() == "scope port rejected by authored constructor",
                "failed construction lost its authored diagnostic");
        require(refusedPort->calls == 1, "failed construction bypassed the real port");
        refusedPort.reset();
        require(rejectedLifetime.expired(), "failed attempt retained an external port");

        auto wrongPort = std::make_shared<Port>(99);
        std::weak_ptr<Port> wrongLifetime = wrongPort;
        const auto wrongSnapshot = [&] {
            const std::array<native::HostedConstructorArgument, 3> inputs{
                wrongPort, firstOffset, native::HostedValue::fromText("wrong type")
            };
            return root.value().constructService(service, inputs);
        }();
        require(wrongSnapshot.is_failure() && wrongPort->calls == 0,
                "malformed scoped input reached a source constructor");
        wrongPort.reset();
        require(wrongLifetime.expired(), "malformed scoped input retained an external port");

        require(!pending.value().completed(), "sibling construction advanced the first scoped task");
        const auto released = root.value().tryStartMethod(tasks, group, "Release", {});
        require(released.is_success() && released.value().result().is_success(), "could not release parked client");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!pending.value().completed() && std::chrono::steady_clock::now() < deadline) {
            tasks.poll();
            std::this_thread::yield();
        }
        require(pending.value().completed() && pending.value().result().is_success() &&
                pending.value().result().value().integer() == 108,
                "parked native task lost its scoped host port after admitting a sibling");

        const auto firstValue = read(root.value(), tasks, first.value());
        const auto secondValue = read(root.value(), tasks, second.value());
        require(firstValue.is_success() && firstValue.value().integer() == 108 &&
                secondValue.is_success() && secondValue.value().integer() == 212 &&
                firstPort->calls == 3 && secondPort->calls == 2,
                "scoped receiver lost or confused its bound port");
        const auto foreignValue = read(foreign.value(), tasks, first.value());
        require(foreignValue.is_failure() && foreignValue.error() == "object belongs to a different owner domain",
                "a foreign root used a borrowed port");
        const auto healthy = root.value().startMethod(tasks, "Canary", {}).result();
        require(healthy.is_success() && healthy.value().integer() == 42,
                "failed scoped construction affected its root");

        require(root.value().retireService(first.value()).is_success(),
                "could not revoke the drained scoped port");
        const auto stale = read(root.value(), tasks, first.value());
        require(stale.is_failure() && stale.error() == "native hosted port has closed",
                "old scoped receiver used its closed port");

        const auto thirdPort = std::make_shared<Port>(303);
        const auto thirdOffset = std::make_shared<Offset>(9);
        const std::array<native::HostedConstructorArgument, 3> thirdInputs{thirdPort, thirdOffset, stamp(4)};
        const auto third = root.value().constructService(service, thirdInputs);
        require(third.is_success(), "replacement scoped port was not admitted");
        const auto replacement = read(root.value(), tasks, third.value());
        require(replacement.is_success() && replacement.value().integer() == 316, "replacement port did not work");
        const auto reused = read(root.value(), tasks, first.value());
        require(reused.is_failure() && reused.error() == "native hosted port has closed" && thirdPort->calls == 2,
                "stale receiver called a replacement port that reused its slot");
        const auto sibling = read(root.value(), tasks, second.value());
        require(sibling.is_success() && sibling.value().integer() == 212 && firstPort->calls == 3,
                "retiring one borrowed port affected a sibling or invoked its external service");
        constexpr std::string_view dependent = "NativeProof.ScopedPorts.Dependent";
        const std::array<native::HostedConstructorArgument, 1> sameGraph{second.value()};
        const auto firstUser = root.value().constructService(dependent, sameGraph);
        const auto secondUser = root.value().constructService(dependent, sameGraph);
        require(firstUser.is_success() && secondUser.is_success() && !firstUser.value().sameObject(secondUser.value()),
                "distinct recipes did not accept their checked native dependency");

        const auto foreignUser = foreign.value().constructService(dependent, sameGraph);
        require(foreignUser.is_failure() && foreignUser.error() == "object belongs to a different owner domain",
                "a native constructor accepted a dependency from another graph");
        const std::array<native::HostedConstructorArgument, 1> wrongClass{firstUser.value()};
        const auto incompatible = root.value().constructService(dependent, wrongClass);
        require(incompatible.is_failure() &&
                incompatible.error() == "native hosted input does not match its checked nominal type",
                "a native constructor accepted a different dependency class");

        model::ContextOwner admission;
        secondPort->revokeOnIdentity = &admission;
        const auto cancelled = [&] {
            const model::InvocationContextScope authorize(admission.capture().permit());
            return root.value().constructService(dependent, sameGraph);
        }();
        secondPort->revokeOnIdentity = nullptr;
        require(cancelled.is_cancelled() && cancelled.error() == "context.stale",
                "the native constructor published after losing its authority");

        const std::array<native::HostedCallArgument, 1> firstUserArgs{firstUser.value()};
        const std::array<native::HostedCallArgument, 1> secondUserArgs{secondUser.value()};
        const auto firstCount = root.value().startMethod(tasks, "Next", firstUserArgs).result();
        const auto secondCount = root.value().startMethod(tasks, "Next", secondUserArgs).result();
        require(firstCount.is_success() && firstCount.value().integer() == 3 &&
                secondCount.is_success() && secondCount.value().integer() == 4,
                "dependent receivers lost their shared state or retained a cancelled constructor's write");

        settleScopedHostCall();
        std::cout << "A checked native composition admitted two typed borrowed ports and an owned scoped snapshot.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
