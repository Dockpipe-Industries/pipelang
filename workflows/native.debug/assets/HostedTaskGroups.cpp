// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_hosted_groups;

void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

auto waiting(const native::HostedInstance& instance, model::TaskScope& scope,
             const std::shared_ptr<model::TaskGroup>& group, std::int64_t identity,
             model::CancellationToken admission = {})
{
    const std::array<native::HostedCallArgument, 2> arguments{identity, model::CancellationToken{}};
    return instance.tryStartMethod(scope, group, "Wait", arguments, std::move(admission));
}

class Observer final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.HostedGroups.Observer"; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature != "Settle(int)" || arguments.size() != 1)
            return Outcome::Failure("unexpected settlement observer invocation");
        seen.push_back(std::get<std::int64_t>(arguments.front()));
        return Outcome::Success(std::int64_t{0});
    }

    std::vector<std::int64_t> seen;
};

int main()
{
    try {
        auto observer = std::make_shared<Observer>();
        const std::array<native::HostedConstructorArgument, 1> inputs{observer};
        const auto bound = native::HostedInstance::bind(proof_hosted_groups, inputs);
        require(bound.is_success(), "binding failed");
        const auto& instance = bound.value();
        model::TaskOptions options;
        options.capacity = 2;
        model::TaskScope scope(options);
        model::TaskScope foreign;
        const auto root = scope.createGroup();
        const auto first = scope.createGroup(root);
        const auto sibling = scope.createGroup(root);

        const auto wrongOwner = waiting(instance, scope, foreign.createGroup(), 8);
        require(wrongOwner.is_failure() && wrongOwner.error() == "task group belongs to a different owner",
                "foreign group did not produce direct refusal");
        const auto unknown = instance.tryStartMethod(scope, first, "Unknown", {});
        require(unknown.is_failure() && unknown.error() == "native hosted method was not found",
                "missing method did not produce direct refusal");
        const auto incomplete = instance.tryStartMethod(scope, first, "Wait", {});
        require(incomplete.is_failure() &&
                incomplete.error() == "native hosted invocation argument count or instance does not match",
                "missing argument did not produce direct refusal");

        model::CancellationSource stopped;
        stopped.cancel();
        const auto refused = waiting(instance, scope, first, 9, stopped.token());
        require(refused.is_cancelled() && scope.pending(root) == 0, "pre-cancelled admission scheduled work");

        auto firstTask = waiting(instance, scope, first, 101);
        auto siblingTask = waiting(instance, scope, sibling, 202);
        require(firstTask.is_success() && siblingTask.is_success(), "separate task groups did not admit one task each");
        require(scope.pending(root) == 2 && scope.pending(first) == 1 && scope.pending(sibling) == 1,
                "native work escaped its owning group or used extra task slots");
        const auto full = waiting(instance, scope, sibling, 10);
        require(full.is_failure() && full.error() == "task capacity exceeded", "capacity refusal did not propagate");

        scope.drainGroup(first);
        require(firstTask.value().completed() && firstTask.value().result().is_cancelled(),
                "closing a group did not cancel and settle its native task");
        require(!siblingTask.value().completed() && scope.pending(root) == 1 && scope.pending(sibling) == 1,
                "closing one group changed its sibling's pending work");
        const auto closed = waiting(instance, scope, first, 11);
        require(closed.is_failure() && closed.error() == "task group is closing", "closed group admitted new work");

        const auto released = instance.tryStartMethod(scope, sibling, "Release", {});
        require(released.is_success() && released.value().result().is_success(), "sibling could not release its wait");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!siblingTask.value().completed() && std::chrono::steady_clock::now() < deadline) {
            scope.poll();
            std::this_thread::yield();
        }
        require(siblingTask.value().completed() && siblingTask.value().result().is_success() &&
                siblingTask.value().result().value().integer() == 202, "sibling's task did not finish independently");

        auto remaining = waiting(instance, scope, sibling, 303);
        require(remaining.is_success() && scope.pending(root) == 1, "root did not observe its remaining child operation");
        scope.drainGroup(root);
        require(remaining.value().completed() && remaining.value().result().is_cancelled() && scope.pending(root) == 0,
                "root cancellation did not drain its descendants");
        const auto count = instance.startMethod(scope, "Settled", {}).result();
        require(count.is_success(), "native settlement snapshot could not be read");
        std::cout << "host";
        for (const auto identity : observer->seen)
            std::cout << ',' << identity;
        std::cout << '\n' << "published";
        for (std::int64_t index = 0; index < count.value().integer(); ++index) {
            const std::array<native::HostedCallArgument, 1> argument{index};
            const auto settlement = instance.startMethod(scope, "Settlement", argument).result();
            require(settlement.is_success(), "native settlement could not be read");
            std::cout << ',' << settlement.value().integer();
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
