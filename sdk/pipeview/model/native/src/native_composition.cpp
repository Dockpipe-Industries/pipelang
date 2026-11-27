// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>
#include <pipelang/di/ScopeState.hpp>
#include <pipelang/model/object.hpp>
#include <pipelang/model/collection.hpp>

#include "NativeLists.hpp"
#include "native_host_projection.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace pipelang::native_app_v19 {

struct HostedComposition::Scope {
    Scope(std::size_t nodes, std::shared_ptr<model::TaskGroup> group)
        : inputs(nodes), cached(nodes), tasks(std::move(group)) {}

    di::ScopeState lifecycle;
    std::vector<std::optional<HostedConstructorArgument>> inputs;
    std::vector<std::optional<HostedValue>> cached;
    di::CleanupStatus cleanup;
    std::shared_ptr<model::TaskGroup> tasks;
    std::shared_ptr<model::TaskState<model::Result<void>>> closeResult;
    bool closeRequested = false;
};

struct HostedComposition::Binding {
    struct Step {
        std::size_t node;
        Scope* owner;
        std::vector<HostedConstructionInput> arguments;
    };

    struct Owned {
        Scope* owner;
        HostedValue receiver;
    };

    Binding(HostedInstance instance, std::span<const HostedCompositionNode> nodes)
        : instance(std::move(instance)), nodes(nodes),
          root(std::make_shared<Scope>(nodes.size(), scheduler.createGroup())) {}

    ~Binding()
    {
        try {
            scheduler.requireOwner();
            for (std::size_t session = 0; session <= di::ScopeLimits::maximumEntries; ++session) {
                (void)closeScope(*root, true);
                if (root->cleanup.phase == di::ScopePhase::Closed)
                    return;
            }
        } catch (...) {
            std::terminate();
        }
    }

    std::size_t lookup(std::string_view service) const
    {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) {
            return node.name == service;
        });
        if (found == nodes.end())
            throw model::ExecutionFailure("native composition service was not found");
        return static_cast<std::size_t>(found - nodes.begin());
    }

    void bindInputs(Scope& owner, HostedLifetime lifetime, std::span<const Input> inputs)
    {
        for (const auto& input : inputs) {
            const auto index = lookup(input.service);
            const auto& node = nodes[index];
            if (node.input == HostedCompositionInput::None || node.lifetime != lifetime || owner.inputs[index])
                throw model::ExecutionFailure("invalid native composition scope input");

            if (node.input == HostedCompositionInput::Borrowed) {
                const auto* service = std::get_if<std::shared_ptr<HostedService>>(&input.argument);
                if (!service || !*service || (*service)->interfaceName() != node.name)
                    throw model::ExecutionFailure("native composition input has the wrong interface");
            } else {
                const auto* snapshot = std::get_if<HostedValue>(&input.argument);
                const auto* integer = std::get_if<std::int64_t>(&input.argument);
                if (!snapshot && !(integer && node.snapshot->kind == OwnedKind::Integer))
                    throw model::ExecutionFailure("native composition input has the wrong snapshot type");
                if (snapshot) {
                    NativeLists lists;
                    HostedInput packed;
                    std::vector<std::uint64_t> words(node.snapshot->words);
                    packed.pack(*node.snapshot, *snapshot, words.data(), &lists);
                }
            }
            owner.inputs[index] = input.argument;
        }

        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            if (node.input != HostedCompositionInput::None && node.lifetime == lifetime && !owner.inputs[index])
                throw model::ExecutionFailure("native composition scope input was not supplied");
        }
    }

    HostedConstructionInput prepare(std::size_t index, Scope& scope, bool rootContext, std::vector<Step>& steps,
                                    std::vector<std::optional<std::size_t>>& staged)
    {
        const auto& node = nodes[index];
        rootContext = rootContext || node.lifetime == HostedLifetime::Singleton;
        if (rootContext && node.lifetime == HostedLifetime::Scoped)
            throw model::ExecutionFailure("native composition scope is unavailable");
        auto& owner = rootContext ? *root : scope;
        if (node.input != HostedCompositionInput::None)
            return *owner.inputs[index];

        if (owner.cached[index])
            return HostedConstructorArgument(*owner.cached[index]);
        if (staged[index])
            return HostedConstructionReference{*staged[index]};

        std::vector<HostedConstructionInput> arguments;
        arguments.reserve(node.dependencyCount);
        for (const auto dependency : std::span(node.dependencies, node.dependencyCount))
            arguments.push_back(prepare(dependency, scope, rootContext, steps, staged));

        const auto step = steps.size();
        if (step == model::ObjectDomain::maximumCells)
            throw model::ExecutionFailure("native composition construction step limit exceeded");
        steps.push_back({index, &owner, std::move(arguments)});
        if (node.lifetime != HostedLifetime::Transient)
            staged[index] = step;
        return HostedConstructionReference{step};
    }

    void admitOwned(std::size_t additions)
    {
        if (additions > model::ObjectDomain::maximumCells - owned.size())
            throw model::ExecutionFailure("native composition owned receiver limit exceeded");
        const auto required = owned.size() + additions;
        if (required > owned.capacity()) {
            const auto grown = std::min(model::ObjectDomain::maximumCells, owned.capacity() * 2);
            owned.reserve(std::max(required, grown));
        }
    }

    static void recordCleanup(di::CleanupStatus& status, const model::Result<void>& outcome)
    {
        --status.pending;
        ++status.attempted;
        if (outcome.is_success()) {
            ++status.succeeded;
        } else {
            ++status.unknown;
            if (outcome.is_cancelled())
                ++status.cancelled;
            else
                ++status.failed;
        }
    }

    static model::Result<void> cleanupOutcome(const Scope& scope)
    {
        if (scope.cleanup.failed != 0)
            return model::Result<void>::Failure("di.cleanup_failed");
        if (scope.cleanup.cancelled != 0)
            return model::Result<void>::Cancelled("di.cleanup_cancelled");
        return model::Result<void>::Success();
    }

    static model::Result<void> beginClose(Scope& scope)
    {
        const auto result = scope.lifecycle.beginClose();
        if (result.is_success() && scope.cleanup.phase == di::ScopePhase::Open)
            scope.cleanup.phase = di::ScopePhase::Closing;
        return result;
    }

    model::Result<void> finishClose(Scope& scope)
    {
        const auto closed = scope.lifecycle.finishClose();
        if (!closed.is_success())
            return closed;
        scope.cached.clear();
        scope.inputs.clear();
        scope.cleanup.phase = di::ScopePhase::Closed;
        if (scope.closeResult)
            scope.closeResult->result = cleanupOutcome(scope);
        if (&scope != root.get()) {
            root->cleanup.attempted += scope.cleanup.attempted;
            root->cleanup.succeeded += scope.cleanup.succeeded;
            root->cleanup.failed += scope.cleanup.failed;
            root->cleanup.cancelled += scope.cleanup.cancelled;
            root->cleanup.unknown += scope.cleanup.unknown;
        }
        return model::Result<void>::Success();
    }

    model::Result<void> closeScope(Scope& scope, bool wait)
    {
        const bool wholeTree = &scope == root.get();
        const auto closing = beginClose(scope);
        if (!closing.is_success())
            return closing;
        if (scope.cleanup.phase == di::ScopePhase::Closed)
            return cleanupOutcome(scope);
        if (wholeTree) {
            for (const auto& child : children) {
                const auto nested = beginClose(*child);
                if (!nested.is_success())
                    return nested;
            }
        }

        if (wait) {
            scheduler.drainGroup(scope.tasks);
        } else {
            scheduler.closeGroup(scope.tasks);
            if (scheduler.pending(scope.tasks) != 0)
                return model::Result<void>::Failure("di.execution_tasks_pending");
        }
        const auto settled = instance.settleFailedConstruction();
        if (!settled.is_success())
            return settled;

        model::EvaluationBudget budget;
        for (std::size_t index = owned.size(); index != 0;) {
            --index;
            const auto& entry = owned[index];
            if (!wholeTree && entry.owner != &scope)
                continue;
            std::optional<model::Result<void>> outcome;
            const auto released = instance.retireService(entry.receiver, budget, outcome);
            if (!released.is_success())
                return released;
            if (outcome)
                recordCleanup(entry.owner->cleanup, *outcome);
            owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(index));
        }

        if (wholeTree) {
            while (!children.empty()) {
                const auto nested = finishClose(*children.back());
                if (!nested.is_success())
                    return nested;
                children.pop_back();
            }
        }
        const auto finished = finishClose(scope);
        return finished.is_success() ? cleanupOutcome(scope) : finished;
    }

    di::CleanupStatus cleanupStatus(const Scope& scope) const
    {
        auto result = scope.cleanup;
        if (&scope == root.get()) {
            for (const auto& child : children) {
                result.pending += child->cleanup.pending;
                result.attempted += child->cleanup.attempted;
                result.succeeded += child->cleanup.succeeded;
                result.failed += child->cleanup.failed;
                result.cancelled += child->cleanup.cancelled;
                result.unknown += child->cleanup.unknown;
            }
        }
        return result;
    }

    static void observe(Scope& scope)
    {
        if (scope.closeResult)
            return;
        scope.closeResult = std::make_shared<model::TaskState<model::Result<void>>>();
        if (scope.cleanup.phase == di::ScopePhase::Closed)
            scope.closeResult->result = cleanupOutcome(scope);
    }

    void processDeferredCloses()
    {
        if (root->closeRequested || root->cleanup.phase == di::ScopePhase::Closing) {
            (void)closeScope(*root, false);
            return;
        }

        auto child = children.begin();
        while (child != children.end()) {
            if ((*child)->closeRequested || (*child)->cleanup.phase == di::ScopePhase::Closing)
                (void)closeScope(**child, false);
            if ((*child)->cleanup.phase == di::ScopePhase::Closed)
                child = children.erase(child);
            else
                ++child;
        }
    }

    HostedInstance instance;
    std::span<const HostedCompositionNode> nodes;
    model::TaskScope scheduler;
    std::shared_ptr<Scope> root;
    std::vector<std::shared_ptr<Scope>> children;
    std::vector<Owned> owned;
};

struct HostedComposition::ScopeLease {
    ScopeLease(const std::shared_ptr<Binding>& binding, const std::shared_ptr<Scope>& scope)
        : binding(binding), scope(scope) {}

    ~ScopeLease()
    {
        const auto owner = binding.lock();
        if (!owner)
            return;
        owner->scheduler.requireOwner();
        try {
            if (scope->cleanup.phase == di::ScopePhase::Closed)
                return;

            scope->closeRequested = true;
            const auto closing = Binding::beginClose(*scope);
            if (!closing.is_success() || !owner->instance.requireIdleSourceSegment().is_success())
                return;
            for (std::size_t session = 0; session <= di::ScopeLimits::maximumEntries; ++session) {
                (void)owner->closeScope(*scope, true);
                if (scope->cleanup.phase == di::ScopePhase::Closed)
                    break;
            }
            if (scope != owner->root && scope->cleanup.phase == di::ScopePhase::Closed)
                std::erase(owner->children, scope);
        } catch (...) {
            // The owning scheduler processes a close request after source reentry unwinds.
        }
    }

    std::weak_ptr<Binding> binding;
    std::shared_ptr<Scope> scope;
};

HostedComposition::HostedComposition(std::shared_ptr<Binding> binding, std::shared_ptr<Scope> scope)
    : binding_(std::move(binding)), scope_(std::move(scope)),
      lease_(std::make_shared<ScopeLease>(binding_, scope_)) {}

HostedComposition::Service::Service(const std::shared_ptr<Scope>& owner, std::size_t node, HostedValue value)
    : owner_(owner), node_(node), value_(std::move(value)) {}

bool HostedComposition::Service::sameObject(const Service& other) const
{
    return value_.sameObject(other.value_);
}

model::Result<void> HostedComposition::requireOpen() const
{
    if (!binding_ || !scope_)
        return model::Result<void>::Failure("native composition binding is empty");
    const auto root = binding_->root->lifecycle.requireOpen();
    if (!root.is_success())
        return root;
    const auto scope = scope_->lifecycle.requireOpen();
    return scope.is_success() ? binding_->instance.requireCompositionAdmission() : scope;
}

model::Result<HostedComposition> HostedComposition::bind(const HostedModel& descriptor,
    std::span<const HostedConstructorArgument> modelArguments, std::span<const Input> singletonInputs)
{
    using Outcome = model::Result<HostedComposition>;
    constexpr auto capabilities = hostedModelGraphCapabilities | hostedModelMultipleTasksCapability |
        hostedModelObjectReferencesCapability;
    const auto version = descriptor.version & ~capabilities;
    if (version != hostedModelWithCompositionPlanAbiVersion && version != hostedModelWithCompositionMethodsAbiVersion &&
        version != hostedModelWithCompositionOperationsAbiVersion && version != hostedModelWithFactoryVariantsAbiVersion)
        return Outcome::Failure("native hosted object has no checked composition plan");

    try {
        auto binding = HostedInstance::bind(descriptor, modelArguments);
        if (binding.is_cancelled())
            return Outcome::Cancelled(binding.error());
        if (binding.is_failure())
            return Outcome::Failure(binding.error());

        const auto nodes = binding.value().compositionNodes();
        auto state = std::make_shared<Binding>(binding.value(), nodes);
        state->bindInputs(*state->root, HostedLifetime::Singleton, singletonInputs);
        return Outcome::Success(HostedComposition(state, state->root));
    } catch (const model::ExecutionFailure& error) {
        return Outcome::Failure(error.what());
    } catch (const std::bad_alloc&) {
        return Outcome::Failure("native composition binding allocation failed");
    }
}

model::Result<HostedComposition> HostedComposition::createScope(std::span<const Input> inputs) const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Outcome = model::Result<HostedComposition>;
    try {
        const auto open = requireOpen();
        if (!open.is_success())
            return Outcome::Failure(open.error());
        model::currentPermit().requireCurrent();
        if (scope != binding->root)
            return Outcome::Failure("native composition scopes are created from the root");
        if (binding->children.size() == di::ScopeLimits::maximumEntries)
            return Outcome::Failure("native composition scope limit exceeded");
        auto child = std::make_shared<Scope>(binding->nodes.size(), binding->scheduler.createGroup(scope->tasks));
        binding->bindInputs(*child, HostedLifetime::Scoped, inputs);
        auto composition = HostedComposition(binding, child);
        binding->children.push_back(std::move(child));
        return Outcome::Success(std::move(composition));
    } catch (const model::OperationCancelled& error) {
        return Outcome::Cancelled(error.what());
    } catch (const model::ExecutionFailure& error) {
        return Outcome::Failure(error.what());
    } catch (const std::bad_alloc&) {
        return Outcome::Failure("native composition scope allocation failed");
    }
}

model::Result<HostedComposition::Service> HostedComposition::resolve(std::string_view service) const
{
    return resolve(service, {});
}

model::Result<HostedComposition::Service> HostedComposition::resolve(
    std::string_view service, model::CancellationToken cancellation) const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Outcome = model::Result<Service>;
    try {
        const auto open = requireOpen();
        if (!open.is_success())
            return Outcome::Failure(open.error());
        model::currentPermit().requireCurrent();

        if (cancellation.is_cancelled())
            return Outcome::Cancelled("native scoped construction cancelled");
        const auto index = binding->lookup(service);
        const auto& node = binding->nodes[index];
        const bool fromRoot = scope == binding->root;
        if ((node.flags & hostedCompositionExported) == 0 ||
            (fromRoot && (node.flags & hostedCompositionRootResolvable) == 0))
            return Outcome::Failure("native composition export is unavailable in this scope");
        const auto& owner = node.lifetime == HostedLifetime::Singleton ? binding->root : scope;
        if (owner->cached[index])
            return Outcome::Success(Service(owner, index, *owner->cached[index]));

        std::vector<Binding::Step> steps;
        std::vector<std::optional<std::size_t>> staged(binding->nodes.size());
        const auto result = binding->prepare(index, *scope, fromRoot, steps, staged);
        binding->admitOwned(steps.size());

        std::vector<HostedConstructionStep> request;
        std::vector<std::shared_ptr<di::CleanupStatus>> cleanupOwners;
        request.reserve(steps.size());
        cleanupOwners.reserve(steps.size());
        for (const auto& step : steps) {
            request.push_back({binding->nodes[step.node].name, step.arguments});
            const auto& owner = step.owner == binding->root.get() ? binding->root : scope;
            cleanupOwners.emplace_back(owner, &owner->cleanup);
        }
        auto completed = binding->instance.constructServices(request, std::move(cancellation), cleanupOwners);
        if (completed.is_cancelled())
            return Outcome::Cancelled(completed.error());
        if (completed.is_failure())
            return Outcome::Failure(completed.error());

        const auto& receivers = completed.value();
        for (std::size_t index = 0; index < steps.size(); ++index) {
            const auto& step = steps[index];
            binding->owned.push_back({step.owner, receivers[index]});
            if (binding->nodes[step.node].lifetime != HostedLifetime::Transient)
                step.owner->cached[step.node] = receivers[index];
        }
        const auto selected = std::get<HostedConstructionReference>(result);
        return Outcome::Success(Service(owner, index, receivers[selected.priorStep]));
    } catch (const model::OperationCancelled& error) {
        return Outcome::Cancelled(error.what());
    } catch (const model::ExecutionFailure& error) {
        return Outcome::Failure(error.what());
    } catch (const std::bad_alloc&) {
        return Outcome::Failure("native composition resolution allocation failed");
    }
}

model::Result<void> HostedComposition::requireAccessible(const Service& service) const
{
    using Status = model::Result<void>;
    const auto owner = service.owner_.lock();
    if (!owner)
        return Status::Failure("di.scope_closed");
    const auto available = owner->lifecycle.requireOpen();
    if (!available.is_success())
        return available;
    if (owner != scope_ && owner != binding_->root)
        return Status::Failure("native composition service belongs to another scope");
    return Status::Success();
}

model::Result<model::Task<HostedValueResult>> HostedComposition::tryStartMethod(
    std::string_view method, std::span<const Argument> arguments, model::CancellationToken cancellation) const
{
    return tryStart(nullptr, method, arguments, std::move(cancellation));
}

model::Result<model::Task<HostedValueResult>> HostedComposition::tryStartServiceMethod(
    const Service& service, std::string_view method, std::span<const Argument> arguments,
    model::CancellationToken cancellation) const
{
    return tryStart(&service, method, arguments, std::move(cancellation));
}

model::Result<model::Task<HostedValueResult>> HostedComposition::tryStart(
    const Service* receiver, std::string_view method, std::span<const Argument> arguments,
    model::CancellationToken cancellation) const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Admission = model::Result<model::Task<HostedValueResult>>;
    try {
        const auto open = requireOpen();
        if (!open.is_success())
            return Admission::Failure(open.error());
        model::currentPermit().requireCurrent();
        if (arguments.size() > maximumArguments)
            return Admission::Failure("native composition method has too many arguments");

        if (receiver) {
            const auto accessible = requireAccessible(*receiver);
            if (!accessible.is_success())
                return Admission::Failure(accessible.error());
        }

        std::array<HostedCallArgument, maximumArguments> admitted;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (const auto* literal = std::get_if<HostedCallArgument>(&arguments[index])) {
                const auto* snapshot = std::get_if<HostedValue>(literal);
                if (snapshot && snapshot->containsObjectReference())
                    return Admission::Failure("native composition requires a scoped service handle for object arguments");
                admitted[index] = *literal;
            } else {
                const auto& service = std::get<Service>(arguments[index]);
                const auto accessible = requireAccessible(service);
                if (!accessible.is_success())
                    return Admission::Failure(accessible.error());
                admitted[index] = service.value_;
            }
        }
        if (receiver)
            return binding->instance.tryStartComponentMethod(binding->scheduler, scope->tasks, receiver->node_,
                receiver->value_, method, {admitted.data(), arguments.size()}, std::move(cancellation));
        return binding->instance.tryStartMethod(binding->scheduler, scope->tasks, method,
            {admitted.data(), arguments.size()}, std::move(cancellation));
    } catch (const model::OperationCancelled& error) {
        return Admission::Cancelled(error.what());
    } catch (const model::ExecutionFailure& error) {
        return Admission::Failure(error.what());
    } catch (const std::bad_alloc&) {
        return Admission::Failure("native composition method allocation failed");
    }
}

model::Result<std::size_t> HostedComposition::poll() const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Outcome = model::Result<std::size_t>;
    if (!binding || !scope)
        return Outcome::Failure("native composition binding is empty");
    try {
        const auto idle = binding->instance.requireIdleSourceSegment();
        if (!idle.is_success())
            return Outcome::Failure(idle.error());
        const auto resumed = binding->scheduler.poll();
        binding->processDeferredCloses();
        return Outcome::Success(resumed);
    } catch (const model::ExecutionFailure& error) {
        return Outcome::Failure(error.what());
    }
}

HostedComposition::CleanupSession::CleanupSession(
    const std::shared_ptr<Binding>& binding, std::shared_ptr<Scope> scope)
    : binding_(binding), scope_(std::move(scope)) {}

model::Result<di::CleanupStatus> HostedComposition::CleanupSession::status() const
{
    using Status = model::Result<di::CleanupStatus>;
    if (!scope_)
        return Status::Failure("native composition cleanup session is empty");

    const auto lifecycle = scope_->lifecycle.status();
    if (!lifecycle.is_success())
        return Status::Failure(lifecycle.error());
    const auto binding = binding_.lock();
    return Status::Success(binding ? binding->cleanupStatus(*scope_) : scope_->cleanup);
}

model::Task<model::Result<void>> HostedComposition::CleanupSession::task() const
{
    if (!scope_)
        throw model::ExecutionFailure("native composition cleanup session is empty");
    const auto lifecycle = scope_->lifecycle.status();
    if (!lifecycle.is_success())
        throw model::ExecutionFailure(lifecycle.error());
    return model::Task<model::Result<void>>(scope_->closeResult);
}

model::Result<di::CleanupStatus> HostedComposition::cleanupStatus() const
{
    using Status = model::Result<di::CleanupStatus>;
    if (!binding_ || !scope_)
        return Status::Failure("native composition binding is empty");

    const auto lifecycle = scope_->lifecycle.status();
    if (!lifecycle.is_success())
        return Status::Failure(lifecycle.error());
    return Status::Success(binding_->cleanupStatus(*scope_));
}

model::Result<HostedComposition::CleanupSession> HostedComposition::observeCleanup() const
{
    using Observation = model::Result<CleanupSession>;
    if (!binding_ || !scope_)
        return Observation::Failure("native composition binding is empty");

    const auto lifecycle = scope_->lifecycle.status();
    if (!lifecycle.is_success())
        return Observation::Failure(lifecycle.error());
    try {
        Binding::observe(*scope_);
        return Observation::Success(CleanupSession(binding_, scope_));
    } catch (const std::bad_alloc&) {
        return Observation::Failure("native composition cleanup observation allocation failed");
    }
}

model::Result<void> HostedComposition::close() const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Status = model::Result<void>;
    if (!binding || !scope)
        return Status::Failure("native composition binding is empty");
    try {
        const auto idle = binding->instance.requireIdleSourceSegment();
        if (!idle.is_success())
            return idle;
        const auto closed = binding->closeScope(*scope, true);
        if (scope != binding->root && scope->cleanup.phase == di::ScopePhase::Closed)
            std::erase(binding->children, scope);
        return closed;
    } catch (const model::ExecutionFailure& error) {
        return Status::Failure(error.what());
    }
}

model::Result<model::Task<model::Result<void>>> HostedComposition::closeAsync() const
{
    const auto binding = binding_;
    const auto scope = scope_;
    using Completion = model::Result<void>;
    using Admission = model::Result<model::Task<Completion>>;
    if (!binding || !scope)
        return Admission::Failure("native composition binding is empty");
    try {
        const auto idle = binding->instance.requireIdleSourceSegment();
        if (!idle.is_success())
            return Admission::Failure(idle.error());
        Binding::observe(*scope);
        const auto closed = binding->closeScope(*scope, false);
        if (scope->cleanup.phase == di::ScopePhase::Closed) {
            scope->closeResult->result = closed;
            if (scope != binding->root)
                std::erase(binding->children, scope);
        }
        return Admission::Success(model::Task<Completion>(scope->closeResult));
    } catch (const model::ExecutionFailure& error) {
        return Admission::Failure(error.what());
    } catch (const std::bad_alloc&) {
        return Admission::Failure("native composition deferred close allocation failed");
    }
}

model::Result<std::size_t> HostedComposition::pending() const
{
    using Outcome = model::Result<std::size_t>;
    if (!binding_ || !scope_)
        return Outcome::Failure("native composition binding is empty");
    try {
        return Outcome::Success(binding_->scheduler.pending(scope_->tasks));
    } catch (const model::ExecutionFailure& error) {
        return Outcome::Failure(error.what());
    }
}

}
