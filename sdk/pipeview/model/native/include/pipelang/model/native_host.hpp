// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "parallel.hpp"
#include "task.hpp"
#include "native_host_value.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace pipelang::di {
struct CleanupStatus;
}

namespace pipelang::model {
class EvaluationBudget;
}

namespace pipelang::native_app_v19 {

// Declare an object emitted with --host-symbol as an extern "C" const HostedModel.
struct HostedModel;
struct HostedMethod;
struct HostedCompositionNode;
class HostedComposition;
using HostedArgument = std::variant<std::int64_t, model::CancellationToken>;
using HostedCallArgument = std::variant<std::int64_t, model::CancellationToken, HostedValue>;
using HostedServiceValue = std::variant<std::int64_t, bool, std::string, HostedValue, model::CancellationToken>;

class HostedService {
public:
    virtual ~HostedService() = default;
    virtual std::string_view interfaceName() const noexcept = 0;
    // Awaited source calls invoke this method on the TaskScope host worker.
    // Service implementations that share state across instances must synchronize it.
    virtual model::Result<HostedServiceValue> invoke(std::string_view signature,
                                                   std::span<const HostedServiceValue> arguments) = 0;
};

using HostedConstructorArgument = std::variant<std::int64_t, std::shared_ptr<HostedService>, HostedValue>;
struct HostedConstructionReference {
    std::size_t priorStep;
};

using HostedConstructionInput = std::variant<HostedConstructorArgument, HostedConstructionReference>;

struct HostedConstructionStep {
    std::string_view service;
    std::span<const HostedConstructionInput> arguments;
};

using HostedResult = model::Result<std::int64_t>;
using HostedValueResult = model::Result<HostedValue>;
using HostedContextOwnerResult = model::Result<std::shared_ptr<model::ContextOwner>>;

class HostedInstance {
public:
    static model::Result<HostedInstance> create(const HostedModel& descriptor,
                                                std::span<const std::int64_t> arguments);
    static model::Result<HostedInstance> bind(const HostedModel& descriptor,
                                              std::span<const HostedConstructorArgument> arguments);
    // Constructor calls share one initialization allowance. Invocation budgets belong to the caller's TaskScope.
    static model::Result<HostedInstance> bind(const HostedModel& descriptor,
                                              std::span<const HostedConstructorArgument> arguments,
                                              std::size_t initializationSteps);
    static model::Result<HostedInstance> bind(const HostedModel& descriptor,
                                              std::span<const HostedConstructorArgument> arguments,
                                              std::size_t initializationSteps, model::ParallelOptions parallel);

    HostedValueResult snapshot() const;
    // Uses a constructor recipe from this binding's checked composition.
    HostedValueResult constructService(std::string_view service,
                                       std::span<const HostedConstructorArgument> arguments) const;
    // Publishes the requested constructors together, with references to earlier steps in the same attempt.
    model::Result<std::vector<HostedValue>> constructServices(std::span<const HostedConstructionStep> steps) const;
    model::Result<std::vector<HostedValue>> constructServices(
        std::span<const HostedConstructionStep> steps, model::CancellationToken cancellation) const;
    // Drain the service's task group before retirement. Tasks in other groups can remain parked.
    // Fails if this service still has an outstanding host call.
    model::Result<void> retireService(const HostedValue& receiver) const;

    // The TaskScope owns scheduling and cancellation. A task retains its
    // receiver and arguments. Concurrent roots on an opted-in binding share
    // the owner thread and TaskScope and yield between execution segments.
    model::Task<HostedResult> start(model::TaskScope& scope, std::span<const HostedArgument> arguments,
                                   model::CancellationToken cancellation = {}) const;
    model::Task<HostedValueResult> startValue(model::TaskScope& scope, std::span<const HostedArgument> arguments,
                                             model::CancellationToken cancellation = {}) const;
    model::Task<HostedValueResult> startWithValues(model::TaskScope& scope,
                                                  std::span<const HostedCallArgument> arguments,
                                                  model::CancellationToken cancellation = {}) const;
    model::Task<HostedValueResult> startMethod(model::TaskScope& scope, std::string_view method,
                                              std::span<const HostedCallArgument> arguments,
                                              model::CancellationToken cancellation = {}) const;

    // The task belongs to the selected group's cancellation and drain subtree.
    // Admission failures are returned before scheduling a source invocation.
    model::Result<model::Task<HostedValueResult>> tryStartMethod(
        model::TaskScope& scope, const std::shared_ptr<model::TaskGroup>& group, std::string_view method,
        std::span<const HostedCallArgument> arguments, model::CancellationToken cancellation = {}) const;

    // An empty method selects the primary entry. The returned handle is confined to the binding's owner thread.
    model::Task<HostedContextOwnerResult> startContextOwner(model::TaskScope& scope, std::string_view method = {},
                                                            model::CancellationToken cancellation = {}) const;

private:
    friend class HostedComposition;
    std::span<const HostedCompositionNode> compositionNodes() const;
    model::Result<void> retireService(const HostedValue& receiver, model::EvaluationBudget& budget,
                                      std::optional<model::Result<void>>& cleanupOutcome) const;
    model::Result<std::vector<HostedValue>> constructServices(
        std::span<const HostedConstructionStep> steps, model::CancellationToken cancellation,
        std::span<const std::shared_ptr<di::CleanupStatus>> cleanupOwners) const;
    model::Result<void> settleFailedConstruction() const;
    model::Result<void> requireIdleSourceSegment() const;
    model::Result<void> requireCompositionAdmission() const;
    model::Result<model::Task<HostedValueResult>> tryStartComponentMethod(
        model::TaskScope& scope, const std::shared_ptr<model::TaskGroup>& group, std::size_t node,
        const HostedValue& receiver, std::string_view name, std::span<const HostedCallArgument> arguments,
        model::CancellationToken cancellation) const;
    struct State;

    explicit HostedInstance(std::shared_ptr<State> state) : state_(std::move(state)) {}
    template<class Value, class Input>
    model::Task<model::Result<Value>> startResult(model::TaskScope& scope, std::span<const Input> arguments,
                                                 model::CancellationToken cancellation,
                                                 const HostedMethod* method = nullptr) const;
    template<class Value, class Input>
    auto invocationFactory(model::TaskScope& scope, std::span<const Input> arguments,
                           model::CancellationToken cancellation, const HostedMethod& method,
                           std::shared_ptr<void> receiver = {}) const;
    template<class Value>
    static model::Coroutine<model::Result<Value>> invoke(std::shared_ptr<State> state, const HostedMethod& method,
                                                       model::TaskScope& scope,
                                                       std::vector<HostedCallArgument> arguments,
                                                       model::CancellationToken cancellation,
                                                       std::shared_ptr<void> receiver);

    std::shared_ptr<State> state_;
};

}
