// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "native_app_abi.hpp"

#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModelWithCompositionMethods proof_root;

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

void verifyCompositionPlan()
{
    require((proof_root.plan.model.model.model.model.model.version & 0xff) == native::hostedModelWithCompositionMethodsAbiVersion,
            "object did not publish its checked composition descriptor");
    const std::span nodes(proof_root.plan.nodes, proof_root.plan.nodeCount);
    require(nodes.size() == 6 && proof_root.plan.model.recipeCount == 3, "unexpected checked graph size");
    const auto find = [&](std::string_view suffix) -> const native::HostedCompositionNode& {
        for (const auto& node : nodes) {
            if (std::string_view(node.name).ends_with(suffix))
                return node;
        }
        throw std::runtime_error("checked service node not found");
    };
    const auto& port = find(".Port");
    const auto& offset = find(".Offset");
    const auto& stamp = find(".Stamp");
    require(port.lifetime == native::HostedLifetime::Scoped &&
            port.input == native::HostedCompositionInput::Borrowed && port.recipe == 0,
            "scoped borrowed input metadata differs");
    require(offset.lifetime == native::HostedLifetime::Singleton &&
            offset.input == native::HostedCompositionInput::Borrowed, "singleton input metadata differs");
    require(stamp.input == native::HostedCompositionInput::Snapshot &&
            stamp.snapshot && stamp.snapshot->kind == native::OwnedKind::Record, "snapshot shape is missing");

    const auto& consumer = find(".Consumer");
    const auto& dependent = find(".Dependent");
    const auto& rootValue = find(".RootValue");
    require(consumer.lifetime == native::HostedLifetime::Scoped && consumer.dependencyCount == 3 &&
            &nodes[consumer.dependencies[0]] == &port && &nodes[consumer.dependencies[1]] == &offset &&
            &nodes[consumer.dependencies[2]] == &stamp, "constructor dependency order differs");
    require(dependent.lifetime == native::HostedLifetime::Transient && dependent.dependencyCount == 1 &&
            &nodes[dependent.dependencies[0]] == &consumer &&
            (dependent.flags & native::hostedCompositionRootResolvable) == 0, "scoped transient metadata differs");
    require(rootValue.lifetime == native::HostedLifetime::Singleton && rootValue.dependencyCount == 1 &&
            &nodes[rootValue.dependencies[0]] == &offset &&
            (rootValue.flags & native::hostedCompositionRootResolvable) != 0, "root singleton edge differs");

    const auto& model = proof_root.plan.model.model.model.model.model;
    require(native::HostedInstance::bind(model, {}).is_success(), "runtime rejected the admitted graph");
    auto altered = proof_root;
    std::vector<native::HostedCompositionNode> invalid(nodes.begin(), nodes.end());
    altered.plan.nodes = invalid.data();
    const auto selected = static_cast<std::size_t>(&consumer - nodes.data());
    const std::uint64_t invalidDependencies[]{selected, consumer.dependencies[1], consumer.dependencies[2]};
    invalid[selected].dependencies = invalidDependencies;
    const auto failure = native::HostedInstance::bind(altered.plan.model.model.model.model.model, {});
    require(failure.is_failure() && failure.error() == "invalid native hosted composition plan",
            "runtime accepted a dependency cycle in the checked descriptor");
}

int main()
{
    try {
        verifyCompositionPlan();
        constexpr std::string_view consumer = "NativeProof.ScopedPorts.Consumer";
        constexpr std::string_view dependent = "NativeProof.ScopedPorts.Dependent";
        const auto instance = native::HostedInstance::bind(proof_root.plan.model.model.model.model.model, {});
        require(instance.is_success(), "could not initialize the checked composition root");
        const auto& root = instance.value();
        const auto port = std::make_shared<Input>("NativeProof.ScopedPorts.Port", "Identity()", 35);
        const auto offset = std::make_shared<Input>("NativeProof.ScopedPorts.Offset", "Read()", 5);
        const auto stamp = native::HostedValue::fromRecord("NativeProof.ScopedPorts.Stamp", {
            {"delta", native::HostedValue::fromInteger(2)}
        });
        const std::array<native::HostedConstructorArgument, 3> arguments{port, offset, stamp};
        const auto existing = root.constructService(consumer, arguments);
        require(existing.is_success(), "existing scoped receiver could not be initialized");

        const std::array<native::HostedConstructionInput, 1> prior{native::HostedConstructorArgument{existing.value()}};
        const std::array<native::HostedConstructionInput, 1> later{native::HostedConstructionReference{3}};
        const std::array<native::HostedConstructionStep, 2> badSteps{{{dependent, prior}, {dependent, later}}};
        const auto refused = root.constructServices(badSteps);
        require(refused.is_failure() && refused.error() == "native scoped construction dependency is unavailable",
                "native construction accepted a reference to an unavailable step");

        const std::array<native::HostedConstructionStep, 2> existingSteps{{{dependent, prior}, {dependent, prior}}};
        const auto shared = root.constructServices(existingSteps);
        require(shared.is_success() && shared.value().size() == 2 &&
                !shared.value()[0].sameObject(shared.value()[1]), "could not construct two services over one dependency");

        model::TaskScope tasks;
        const std::array<native::HostedCallArgument, 1> readFirst{shared.value()[0]};
        const auto count = root.startMethod(tasks, "Next", readFirst).result();
        require(count.is_success() && count.value().integer() == 3,
                "failed multi-recipe resolution left writes to an existing dependency");

        const std::array<native::HostedConstructionInput, 3> freshArguments{arguments[0], arguments[1], arguments[2]};
        const std::array<native::HostedConstructionInput, 1> pending{native::HostedConstructionReference{0}};
        const std::array<native::HostedConstructionStep, 3> newSteps{{
            {consumer, freshArguments}, {dependent, pending}, {dependent, pending}
        }};
        const auto admitted = root.constructServices(newSteps);
        require(admitted.is_success() && admitted.value().size() == 3, "construction could not use a staged native dependency");
        const std::array<native::HostedCallArgument, 1> readFresh{admitted.value()[2]};
        const auto fresh = root.startMethod(tasks, "Next", readFresh).result();
        require(fresh.is_success() && fresh.value().integer() == 3, "published fresh graph lost its shared source state");
        std::cout << "Checked native constructors published staged dependencies and rolled back a failed batch.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
