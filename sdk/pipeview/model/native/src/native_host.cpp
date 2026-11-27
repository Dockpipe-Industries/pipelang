// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include "native_app_execution.hpp"
#include "native_host_projection.hpp"
#include "NativeLists.hpp"
#include "NativeInteger.hpp"
#include "NativeHostedConstruction.hpp"
#include <pipelang/di/CleanupStatus.hpp>

#include <pipelang/model/source_call.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <map>
#include <set>
#include <limits>
#include <type_traits>

namespace pipelang::native_app_v19 {

namespace {

bool retainedScopedInput(const OwnedShape& shape, std::size_t depth = 0)
{
    if (depth >= 128 || shape.words == 0 || shape.words > maximumFrameWords)
        return false;

    switch (shape.kind) {
    case OwnedKind::Integer:
    case OwnedKind::UInt32:
    case OwnedKind::Boolean:
    case OwnedKind::Float:
    case OwnedKind::Float32:
    case OwnedKind::Enum:
        return true;
    case OwnedKind::Text:
        return shape.words == sizeof(TextView) / sizeof(std::uint64_t);
    case OwnedKind::ByteBuffer:
        return shape.words == sizeof(ByteView) / sizeof(std::uint64_t);
    case OwnedKind::ObjectReference:
        return depth == 0 && shape.words == 1 && shape.name && *shape.name;
    case OwnedKind::List:
    case OwnedKind::Array:
        if (shape.words != sizeof(ListView) / sizeof(std::uint64_t))
            return false;
        return shape.fields && shape.fieldCount == 1 && shape.fields[0].shape &&
            retainedScopedInput(*shape.fields[0].shape, depth + 1);
    case OwnedKind::Queue:
    case OwnedKind::Stack:
        if (shape.words != sizeof(SequenceView) / sizeof(std::uint64_t))
            return false;
        return shape.fields && shape.fieldCount == 1 && shape.fields[0].shape &&
            retainedScopedInput(*shape.fields[0].shape, depth + 1);
    case OwnedKind::HashSet:
        if (shape.words != sizeof(SetView) / sizeof(std::uint64_t))
            return false;
        return shape.fields && shape.fieldCount == 1 && shape.fields[0].shape &&
            retainedScopedInput(*shape.fields[0].shape, depth + 1);
    case OwnedKind::Dictionary:
        if (shape.words != sizeof(DictionaryView) / sizeof(std::uint64_t) || shape.fieldCount != 3 || !shape.fields)
            return false;
        return std::all_of(shape.fields, shape.fields + shape.fieldCount, [&](const auto& field) {
            return field.shape && retainedScopedInput(*field.shape, depth + 1);
        });
    case OwnedKind::Pair:
    case OwnedKind::Record:
        if (shape.fieldCount > shape.words || (shape.fieldCount && !shape.fields))
            return false;
        for (const auto& field : std::span(shape.fields, shape.fieldCount)) {
            if (!field.name || !field.shape || field.offsetWords > shape.words ||
                field.shape->words > shape.words - field.offsetWords || !retainedScopedInput(*field.shape, depth + 1))
                return false;
        }
        return true;
    default:
        return false;
    }
}

bool validCompositionPlan(const HostedModelWithCompositionPlan& plan, std::span<const HostedConstructorRecipe> recipes)
{
    if (!plan.nodes || plan.nodeCount < recipes.size() || plan.nodeCount > model::ObjectDomain::maximumCells)
        return false;

    constexpr auto allowedFlags = hostedCompositionExported | hostedCompositionRootResolvable | hostedCompositionScopeResolvable;
    const std::span nodes(plan.nodes, plan.nodeCount);
    std::set<std::string_view> names;
    std::size_t recipeCount = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (!node.name || !*node.name || !names.insert(node.name).second ||
            static_cast<std::uint64_t>(node.lifetime) > static_cast<std::uint64_t>(HostedLifetime::Transient) ||
            (node.flags & ~allowedFlags) != 0 || node.dependencyCount > maximumArguments ||
            (node.dependencyCount != 0 && !node.dependencies))
            return false;

        bool rootResolvable = node.lifetime != HostedLifetime::Scoped;
        for (const auto dependency : std::span(node.dependencies, node.dependencyCount)) {
            if (dependency >= index)
                return false;
            rootResolvable = rootResolvable && (nodes[dependency].flags & hostedCompositionRootResolvable) != 0;
        }
        if (rootResolvable != ((node.flags & hostedCompositionRootResolvable) != 0) ||
            (node.lifetime == HostedLifetime::Singleton && !rootResolvable))
            return false;

        switch (node.input) {
        case HostedCompositionInput::None:
            if (recipeCount >= recipes.size() || node.recipe != recipeCount + 1 || node.snapshot ||
                node.name != std::string_view(recipes[recipeCount].name) ||
                node.dependencyCount != recipes[recipeCount].parameterCount)
                return false;
            ++recipeCount;
            break;
        case HostedCompositionInput::Snapshot:
        case HostedCompositionInput::Borrowed:
            if (node.recipe != 0 || node.dependencyCount != 0 || node.lifetime == HostedLifetime::Transient ||
                (node.flags & hostedCompositionExported) != 0)
                return false;
            if (node.input == HostedCompositionInput::Snapshot) {
                if (!node.snapshot || node.snapshot->kind == OwnedKind::ObjectReference || !retainedScopedInput(*node.snapshot))
                    return false;
            } else if (node.snapshot) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return recipeCount == recipes.size();
}

bool validMethod(const HostedMethod& method, bool sharedArguments, bool collectionArguments)
{
    if (!method.entry || !validAsyncFunction(*method.entry))
        return false;
    const auto& entry = *method.entry;
    const auto payloadWords = method.successShape ? method.successShape->words : 0;
    if (payloadWords != entry.resultWords - resultHeaderWords)
        return false;
    const auto cancellationArguments = cancellationArgumentMask(entry);
    if (!method.argumentShapes)
        return method.indirectArguments == 0;
    if ((method.indirectArguments >> entry.argumentCount) != 0 ||
        (method.indirectArguments & cancellationArguments) != 0)
        return false;

    std::size_t words = 0;
    for (std::size_t index = 0; index < entry.argumentCount; ++index) {
        const auto mask = std::uint64_t{1} << index;
        const auto* shape = method.argumentShapes[index];
        if ((cancellationArguments & mask) != 0) {
            if (shape)
                return false;
            continue;
        }
        if (!shape || shape->words == 0 || shape->words > maximumFrameWords - words ||
            ((method.indirectArguments & mask) == 0 && shape->words != 1))
            return false;
        if (shape->kind == OwnedKind::Reference) {
            const bool supported = hostedSemaphoreShape(*shape) || hostedIntegerQueueShape(*shape) ||
                hostedLockShape(*shape) || hostedAtomicShape(*shape) ||
                (collectionArguments && (hostedConcurrentSequenceShape(*shape) || hostedConcurrentDictionaryShape(*shape)));
            if (!sharedArguments || !supported || (method.indirectArguments & mask) != 0)
                return false;
        }
        words += shape->words;
    }
    return true;
}

}

struct HostedInstance::State {
    explicit State(const HostedModel& descriptor)
        : entry(*descriptor.entry), primary{nullptr, &entry, descriptor.successShape, nullptr, 0},
          receiver(receiverHeaderWords + descriptor.receiverWords),
          receiverStorage(receiver.data(), ownedReceivers, constructorCollections,
                          (descriptor.version & hostedModelGraphCapability) != 0,
                          (descriptor.version & hostedModelPreciseRootsCapability) != 0)
    {
        receiver[receiverSizeWord] = descriptor.receiverWords;
    }

    static void* allocateReceiver(void* context, std::uint64_t words, std::uint64_t tag) noexcept
    {
        auto& construction = *static_cast<NativeHostedConstruction*>(context);
        return construction.storage.constructionReceivers().allocate(words, tag);
    }

    static model::Result<void> runResourceCleanup(
        NativeHostedConstruction& execution, HostedCleanup cleanup, void* receiver)
    {
        using Status = model::Result<void>;
        auto allowance = execution.budget.cleanup();
        execution.budget.step(); // A pre-claim budget failure leaves this operation unattempted.
        execution.failure = {};
        execution.pendingFailure = {};
        execution.sourceDepth = 1;
        try {
            model::InvocationContextScope cleanupContext({});
            const model::CancellationToken token;
            ResultHeader result{};
            const auto status = cleanup(&result, receiver, &execution, State::allocateReceiver, &token);
            if (execution.failure)
                std::rethrow_exception(execution.failure);
            if (status != 0)
                return Status::Failure("native resource cleanup invocation failed");
            if (result.tag == ResultTag::Success)
                return Status::Success();
            if (result.tag != ResultTag::Cancelled && result.tag != ResultTag::Failure)
                return Status::Failure("native resource cleanup returned an invalid outcome");
            if (result.error.size > model::maximumStringBytes || (result.error.size != 0 && !result.error.bytes))
                return Status::Failure("native resource cleanup returned an invalid error");
            const auto message = result.error.size == 0
                ? std::string{} : std::string(result.error.bytes, result.error.size);
            if (result.tag == ResultTag::Cancelled)
                return Status::Cancelled(message);
            return Status::Failure(message);
        } catch (const model::OperationCancelled& error) {
            return Status::Cancelled(model::safeErrorMessage(error));
        } catch (const std::exception& error) {
            return Status::Failure(model::safeErrorMessage(error));
        } catch (...) {
            return Status::Failure("native resource cleanup failed unexpectedly");
        }
    }

    const HostedMethod* findMethod(std::string_view name) const
    {
        const auto method = std::find_if(methods.begin(), methods.end(), [&](const HostedMethod& candidate) {
            return name == candidate.name;
        });
        return method == methods.end() ? nullptr : &*method;
    }

    AsyncFunction entry;
    HostedMethod primary;
    std::span<const HostedMethod> methods;
    struct ComponentLease {
        HostedValue receiver;
        std::vector<std::size_t> serviceSlots;
        std::shared_ptr<const ScopedServiceUse> methodUse;
        const HostedConstructorRecipe* recipe;
        const HostedFactoryVariant* variant;
        std::optional<model::Result<void>> cleanupOutcome;
    };

    struct Construction;
    std::span<const HostedConstructorRecipe> constructorRecipes;
    std::span<const HostedFactory> factories;
    std::span<const HostedCleanup> cleanups;
    std::span<const HostedFactoryVariants> factoryVariants;

    HostedCleanup cleanupFor(const HostedConstructorRecipe& recipe, const HostedFactoryVariant* variant) const
    {
        if (variant)
            return variant->cleanup;
        const auto index = static_cast<std::size_t>(&recipe - constructorRecipes.data());
        return cleanups.empty() ? nullptr : cleanups[index];
    }
    std::span<const HostedCompositionNode> composition;
    std::span<const HostedCompositionMethods> compositionMethods;
    HostServiceBindings services;
    std::map<void*, ComponentLease> components;
    std::uint32_t nextScopedGeneration = 1;
    HostedInput constructorValues;
    NativeLists constructorCollections;
    std::vector<std::uint64_t> receiver;
    NativeReceivers ownedReceivers;
    NativeReceiverStorage receiverStorage;
    const OwnedShape* snapshotShape = nullptr;
    const std::thread::id ownerThread = std::this_thread::get_id();
    const model::TaskScope* activeScope = nullptr;
    std::size_t activeInvocations = 0;
    std::atomic_flag running{};
    bool scopedConstructionActive = false;
    bool unwindingResources = false;
    std::shared_ptr<Construction> failedConstruction;
};

model::Result<HostedInstance> HostedInstance::create(const HostedModel& descriptor,
                                                     std::span<const std::int64_t> arguments)
{
    if (arguments.size() > maximumArguments)
        return model::Result<HostedInstance>::Failure("native hosted constructor argument count does not match");
    std::vector<HostedConstructorArgument> captured(arguments.begin(), arguments.end());
    return bind(descriptor, captured);
}

model::Result<HostedInstance> HostedInstance::bind(const HostedModel& descriptor,
                                                   std::span<const HostedConstructorArgument> arguments)
{
    return bind(descriptor, arguments, model::defaultEvaluationSteps);
}

model::Result<HostedInstance> HostedInstance::bind(const HostedModel& descriptor,
    std::span<const HostedConstructorArgument> arguments, std::size_t initializationSteps)
{
    return bind(descriptor, arguments, initializationSteps, {});
}

model::Result<HostedInstance> HostedInstance::bind(const HostedModel& descriptor,
    std::span<const HostedConstructorArgument> arguments, std::size_t initializationSteps, model::ParallelOptions parallel)
{
    using InstanceResult = model::Result<HostedInstance>;
    if (parallel.maximumDegreeOfParallelism == 0 ||
        parallel.maximumDegreeOfParallelism > model::ParallelOptions::maximumDegree)
        return InstanceResult::Failure("parallel degree must be between 1 and 32");
    const auto features = descriptor.version & hostedModelGraphCapabilities;
    const auto version = descriptor.version & ~(hostedModelGraphCapabilities | hostedModelMultipleTasksCapability |
                                                 hostedModelObjectReferencesCapability |
                                                 hostedModelSharedArgumentsCapability |
                                                 hostedModelCollectionArgumentsCapability |
                                                 hostedModelSharedConstructorCapability);
    const bool multipleTasks = (descriptor.version & hostedModelMultipleTasksCapability) != 0;
    const bool objectReferences = (descriptor.version & hostedModelObjectReferencesCapability) != 0;
    const bool sharedArguments = (descriptor.version & hostedModelSharedArgumentsCapability) != 0;
    const bool collectionArguments = (descriptor.version & hostedModelCollectionArgumentsCapability) != 0;
    const bool sharedConstructor = (descriptor.version & hostedModelSharedConstructorCapability) != 0;
    if (features == hostedModelPreciseRootsCapability)
        return InstanceResult::Failure("invalid native hosted graph capability");
    if (objectReferences && features != hostedModelGraphCapabilities)
        return InstanceResult::Failure("native hosted object references require a precise receiver graph");
    const bool hasOwnedArguments = version == hostedModelWithArgumentsAbiVersion;
    const bool hasFactoryVariants = version == hostedModelWithFactoryVariantsAbiVersion;
    const bool hasOperations = version == hostedModelWithCompositionOperationsAbiVersion || hasFactoryVariants;
    const bool hasCompositionMethods = version == hostedModelWithCompositionMethodsAbiVersion || hasOperations;
    const bool hasPlan = version == hostedModelWithCompositionPlanAbiVersion || hasCompositionMethods;
    const bool hasComposition = version == hostedModelWithCompositionAbiVersion || hasPlan;
    const bool hasSnapshotLayout = version == hostedModelWithSnapshotAbiVersion || hasComposition;
    const bool hasMethods = version == hostedModelWithMethodsAbiVersion || hasSnapshotLayout;
    if ((!hasMethods && !hasOwnedArguments && version != hostedModelAbiVersion) ||
        !descriptor.construct || !descriptor.entry ||
        descriptor.constructorArguments > maximumArguments || descriptor.receiverWords > maximumReceiverWords ||
        !validAsyncFunction(*descriptor.entry) ||
        (descriptor.constructorArguments != 0 && !descriptor.parameters))
        return InstanceResult::Failure("invalid native hosted model descriptor");
    const auto payloadWords = descriptor.successShape ? descriptor.successShape->words : 0;
    if (payloadWords != descriptor.entry->resultWords - resultHeaderWords)
        return InstanceResult::Failure("invalid native hosted model descriptor");
    if (arguments.size() != descriptor.constructorArguments)
        return InstanceResult::Failure("native hosted constructor argument count does not match");

    const auto initialization = model::currentPermit();
    model::EvaluationBudget budget;
    try {
        initialization.requireCurrent();
        budget = model::EvaluationBudget::forInitialization(initializationSteps);
    } catch (const model::OperationCancelled& cancellation) {
        return InstanceResult::Cancelled(cancellation.what());
    } catch (const model::ExecutionFailure& failure) {
        return InstanceResult::Failure(failure.what());
    }

    auto state = std::make_shared<State>(descriptor);
    state->receiverStorage.permitsMultipleTasks = multipleTasks;
    state->receiverStorage.setParallelOptions(parallel);
    if (objectReferences)
        state->receiverStorage.enableObjectReferences();
    if (hasOwnedArguments || hasMethods) {
        const auto& extended = reinterpret_cast<const HostedModelWithArguments&>(descriptor);
        state->primary.argumentShapes = extended.argumentShapes;
        state->primary.indirectArguments = extended.indirectArguments;
        if ((hasOwnedArguments && !extended.argumentShapes) || !validMethod(state->primary, sharedArguments, collectionArguments))
            return InstanceResult::Failure("invalid native hosted invocation descriptor");
    }
    if (hasMethods) {
        const auto& extended = reinterpret_cast<const HostedModelWithMethods&>(descriptor);
        if (extended.methodCount == 0) {
            if (!hasSnapshotLayout || extended.methods)
                return InstanceResult::Failure("invalid native hosted method table");
        } else if (!extended.methods || extended.methodCount < 2 || extended.methodCount > maximumHostedMethods) {
            return InstanceResult::Failure("invalid native hosted method table");
        }
        state->methods = {extended.methods, extended.methodCount};
        for (const auto& method : state->methods) {
            if (!method.name || !*method.name || !validMethod(method, sharedArguments, collectionArguments))
                return InstanceResult::Failure("invalid native hosted method descriptor");
        }
    }
    if (hasSnapshotLayout) {
        const auto& extended = reinterpret_cast<const HostedModelWithSnapshot&>(descriptor);
        const auto* shape = extended.snapshot;
        if (!shape && !hasComposition)
            return InstanceResult::Failure("invalid native hosted snapshot descriptor");
        if (shape) {
            if (shape->kind != OwnedKind::Record || shape->words != descriptor.receiverWords || !shape->name ||
                shape->fieldCount > shape->words || (shape->fieldCount != 0 && !shape->fields))
                return InstanceResult::Failure("invalid native hosted snapshot descriptor");
            for (const auto& field : std::span(shape->fields, shape->fieldCount)) {
                if (!field.name || !field.shape || field.offsetWords > shape->words ||
                    field.shape->words > shape->words - field.offsetWords)
                    return InstanceResult::Failure("invalid native hosted snapshot field");
            }
            state->snapshotShape = shape;
            state->receiverStorage.exposesSnapshot = true;
        }
    }
    if (hasComposition) {
        const auto& extended = reinterpret_cast<const HostedModelWithComposition&>(descriptor);
        if (!objectReferences || !extended.recipes || extended.recipeCount == 0 ||
            extended.recipeCount > model::ObjectDomain::maximumCells)
            return InstanceResult::Failure("invalid native hosted composition descriptor");

        std::set<std::string_view> names;
        const std::span recipes(extended.recipes, extended.recipeCount);
        if (hasOperations) {
            const auto& operations = reinterpret_cast<const HostedModelWithCompositionOperations&>(descriptor);
            if (!operations.factories && !operations.cleanups)
                return InstanceResult::Failure("invalid native hosted composition operation tables");
            if (operations.factories)
                state->factories = {operations.factories, recipes.size()};
            if (operations.cleanups)
                state->cleanups = {operations.cleanups, recipes.size()};
        }
        if (hasFactoryVariants) {
            const auto& variantModel = reinterpret_cast<const HostedModelWithFactoryVariants&>(descriptor);
            if (!variantModel.variants || state->factories.empty())
                return InstanceResult::Failure("invalid native hosted factory subtype tables");
            state->factoryVariants = {variantModel.variants, recipes.size()};
        }
        for (std::size_t index = 0; index < recipes.size(); ++index) {
            const auto& recipe = recipes[index];
            const auto factory = state->factories.empty() ? nullptr : state->factories[index];
            if (!recipe.name || !*recipe.name || !names.insert(recipe.name).second ||
                static_cast<bool>(factory) == static_cast<bool>(recipe.construct) ||
                !recipe.type || recipe.type->kind != OwnedKind::ObjectReference ||
                recipe.type->words != 1 || !recipe.type->name || recipe.receiverWords > maximumReceiverWords ||
                recipe.parameterCount > maximumArguments || (recipe.parameterCount && !recipe.parameters))
                return InstanceResult::Failure("invalid native hosted constructor recipe");

            if (hasFactoryVariants) {
                const auto& variants = state->factoryVariants[index];
                if ((!variants.entries && variants.count != 0) || (variants.entries && variants.count == 0) ||
                    variants.count > model::ObjectDomain::maximumCells || (variants.count != 0 && !factory))
                    return InstanceResult::Failure("invalid native hosted factory subtype table");
                std::set<std::uint64_t> tags;
                for (const auto& variant : std::span(variants.entries, variants.count)) {
                    const auto cleanup = state->cleanups.empty() ? nullptr : state->cleanups[index];
                    const auto& table = variant.methods;
                    if (variant.receiverTag == recipe.receiverTag || !tags.insert(variant.receiverTag).second ||
                        variant.receiverWords < recipe.receiverWords || variant.receiverWords > maximumReceiverWords ||
                        static_cast<bool>(cleanup) != static_cast<bool>(variant.cleanup) ||
                        (table.methodCount == 0 && table.methods) ||
                        (table.methodCount != 0 && !table.methods) || table.methodCount > maximumHostedMethods)
                        return InstanceResult::Failure("invalid native hosted factory subtype descriptor");
                    std::set<std::string_view> methods;
                    for (const auto& method : std::span(table.methods, table.methodCount)) {
                        if (!method.name || !*method.name || !methods.insert(method.name).second ||
                            !validMethod(method, sharedArguments, collectionArguments))
                            return InstanceResult::Failure("invalid native hosted factory subtype method");
                    }
                }
            }

            for (const auto& parameter : std::span(recipe.parameters, recipe.parameterCount)) {
                if (parameter.service ? (*parameter.service == '\0' || parameter.value)
                                      : (!parameter.value || !retainedScopedInput(*parameter.value)))
                    return InstanceResult::Failure("invalid native hosted constructor recipe parameter");
            }
        }
        if (hasPlan) {
            const auto& embedded = reinterpret_cast<const HostedModelWithCompositionPlan&>(descriptor);
            if (!validCompositionPlan(embedded, recipes))
                return InstanceResult::Failure("invalid native hosted composition plan");
            state->composition = {embedded.nodes, embedded.nodeCount};
            if (hasCompositionMethods) {
                const auto& withMethods = reinterpret_cast<const HostedModelWithCompositionMethods&>(descriptor);
                if (!withMethods.tables && !hasOperations)
                    return InstanceResult::Failure("invalid native hosted service method tables");
                if (withMethods.tables)
                    state->compositionMethods = {withMethods.tables, embedded.nodeCount};
                for (std::size_t index = 0; index < state->compositionMethods.size(); ++index) {
                    const auto& node = embedded.nodes[index];
                    const auto& table = withMethods.tables[index];
                    if (table.methodCount == 0) {
                        if (table.methods)
                            return InstanceResult::Failure("invalid native hosted service method table");
                        continue;
                    }
                    if (!table.methods || table.methodCount > maximumHostedMethods || node.recipe == 0 ||
                        (node.flags & hostedCompositionExported) == 0)
                        return InstanceResult::Failure("invalid native hosted service method table");

                    std::set<std::string_view> names;
                    for (const auto& method : std::span(table.methods, table.methodCount)) {
                        if (!method.name || !*method.name || !names.insert(method.name).second ||
                            !validMethod(method, sharedArguments, collectionArguments))
                            return InstanceResult::Failure("invalid native hosted service method descriptor");
                    }
                }
            }
        }
        state->constructorRecipes = recipes;
    }
    std::array<std::uint64_t, maximumArguments> scalars{};
    std::array<std::vector<std::uint64_t>, maximumArguments> values;
    std::array<const std::uint64_t*, maximumArguments> inputs{};
    try {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& parameter = descriptor.parameters[index];
            inputs[index] = &scalars[index];
            if (parameter.service) {
                if (parameter.value)
                    return InstanceResult::Failure("invalid native hosted constructor descriptor");
                const auto* service = std::get_if<std::shared_ptr<HostedService>>(&arguments[index]);
                if (!service || !*service || (*service)->interfaceName() != parameter.service)
                    return InstanceResult::Failure(
                        "native hosted constructor service does not implement " + std::string(parameter.service));
                state->services.push_back({*service, 0, {}});
                scalars[index] = state->services.size();
                continue;
            }
            if (!parameter.value || parameter.value->words == 0 || parameter.value->words > maximumFrameWords)
                return InstanceResult::Failure("invalid native hosted constructor descriptor");
            const auto* integer = std::get_if<std::int64_t>(&arguments[index]);
            if (integer && parameter.value->kind == OwnedKind::Integer) {
                scalars[index] = static_cast<std::uint64_t>(*integer);
                continue;
            }
            const auto* value = std::get_if<HostedValue>(&arguments[index]);
            if (!value)
                return InstanceResult::Failure(
                    "native hosted constructor requires a value of type " + std::string(parameter.value->name));
            if (parameter.value->kind == OwnedKind::Reference) {
                if (!sharedConstructor)
                    return InstanceResult::Failure("invalid native hosted constructor shared argument");
                auto& storage = state->receiverStorage;
                const auto borrowed = borrowHostedArgument(*parameter.value, *value,
                    {storage.semaphores, storage.locks, storage.atomics, storage.sequences, storage.dictionaries});
                if (storage.hasPreciseRoots())
                    scalars[index] = storage.retainSynchronization(nullptr, borrowed.kind, borrowed.handle);
                else
                    scalars[index] = hostedSynchronizationTag | borrowed.handle;
                continue;
            }
            auto& storage = values[index];
            storage.resize(parameter.value->words);
            state->constructorValues.pack(*parameter.value, *value, storage.data(), &state->constructorCollections);
            inputs[index] = storage.data();
        }
    } catch (const model::ExecutionFailure& failure) {
        return InstanceResult::Failure(failure.what());
    }
    NativeHostedConstruction construction{state->receiverStorage, state->services, state->constructorValues,
        state->constructorCollections, std::move(budget)};
    const auto status = descriptor.construct(state->receiver.data(), inputs.data(), &construction, State::allocateReceiver);
    if (status == 1)
        return InstanceResult::Failure("native hosted receiver allocation failed");
    if (status == 3) {
        try {
            if (construction.failure)
                std::rethrow_exception(construction.failure);
        } catch (const model::OperationCancelled& cancellation) {
            return InstanceResult::Cancelled(cancellation.what());
        } catch (const model::ExecutionFailure& failure) {
            return InstanceResult::Failure(failure.what());
        } catch (const std::bad_alloc&) {
            return InstanceResult::Failure("native hosted synchronization allocation failed");
        }
        return InstanceResult::Failure("native hosted synchronization construction failed");
    }
    if (status == 4)
        return InstanceResult::Failure("context.null_owner");
    if (status != 0)
        return InstanceResult::Failure("native hosted construction read an uninitialized receiver field");
    try {
        state->receiverStorage.retainConstructionByteFields(state->constructorValues);
        state->receiverStorage.finishBinding();
        initialization.requireCurrent();
    } catch (const model::OperationCancelled& cancellation) {
        return InstanceResult::Cancelled(cancellation.what());
    } catch (const model::ExecutionFailure& failure) {
        return InstanceResult::Failure(failure.what());
    } catch (const std::bad_alloc&) {
        return InstanceResult::Failure("native hosted receiver binding allocation failed");
    }
    return InstanceResult::Success(HostedInstance(std::move(state)));
}

namespace {

template<class Initialize>
std::int32_t createHostedSynchronization(void* context, ReceiverSynchronizationKind kind,
                                         std::uint64_t* output, const Initialize& initialize) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        auto& storage = construction.storage;
        if (storage.hasPreciseRoots())
            *output = storage.retainSynchronization(nullptr, kind, initialize(storage));
        else
            *output = hostedSynchronizationTag | initialize(storage);
        if (construction.transactions.constructing()) {
            try {
                construction.synchronization.emplace_back(kind, *output);
            } catch (...) {
                storage.discardHostedSynchronization(kind, *output);
                throw;
            }
        }
    });
}

}

extern "C" std::int32_t pipelang_native_v19_hosted_enter_source_call(void* context) noexcept
{
    return withHostedConstruction(context, [](NativeHostedConstruction& construction) {
        model::SourceCallContext::requireCapacity(construction.sourceDepth);
        ++construction.sourceDepth;
    });
}

extern "C" void pipelang_native_v19_hosted_leave_source_call(void* context) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(context);
    --construction.sourceDepth;
}

namespace {

std::int32_t beginHostedConstruction(void* context, bool tracksDepth) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        if (tracksDepth)
            model::SourceCallContext::requireCapacity(construction.sourceDepth);
        ReceiverTransactions::ConstructionExit exit{};
        exit.tracksDepth = tracksDepth;
        exit.hostedSynchronizationCheckpoint = construction.synchronization.size();
        construction.transactions.beginConstruction(construction.lists, exit);
        if (tracksDepth)
            ++construction.sourceDepth;
    });
}

}

extern "C" std::int32_t pipelang_native_v19_hosted_begin_construction(void* context) noexcept
{
    return beginHostedConstruction(context, false);
}

extern "C" std::int32_t pipelang_native_v19_hosted_begin_tracked_construction(void* context) noexcept
{
    return beginHostedConstruction(context, true);
}

extern "C" void pipelang_native_v19_hosted_finish_construction(void* context, std::int32_t status) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(context);
    const auto exit = construction.transactions.finishConstruction(status == 0);
    if (status != 0) {
        while (construction.synchronization.size() > exit.hostedSynchronizationCheckpoint) {
            const auto [kind, handle] = construction.synchronization.back();
            construction.storage.discardHostedSynchronization(kind, handle);
            construction.synchronization.pop_back();
        }
    }
    if (!construction.transactions.constructing())
        construction.synchronization.clear();
    if (exit.tracksDepth)
        --construction.sourceDepth;
}

extern "C" std::int32_t pipelang_native_v19_hosted_track_receiver(void* context, void* receiver) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        construction.transactions.track(receiver);
    });
}

extern "C" std::int32_t pipelang_native_v19_charge_hosted_initialization(void* context, std::size_t steps) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction& construction) {
        construction.budget.chargeSteps(steps);
    });
}

extern "C" std::int32_t pipelang_native_v19_hosted_integer(void* context, std::int32_t operation,
    std::int64_t left, std::int64_t right, std::int64_t* output) noexcept
{
    return withHostedConstruction(context, [&](NativeHostedConstruction&) {
        *output = checkedInteger(static_cast<IntegerOperation>(operation), left, right);
    });
}

extern "C" std::int32_t pipelang_native_v19_record_constructed_receiver_field(
    void* context, const void* receiver, const std::uint64_t* field, const OwnedShape* shape,
    std::int32_t rawKind) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(context);
    try {
        auto& storage = construction.storage;
        if (!storage.hasPreciseRoots() || !storage.text.owns(receiver))
            throw model::ExecutionFailure("native constructor field does not belong to its graph");
        if (shape)
            storage.recordConstructedValue(receiver, field, *shape);
        else if (rawKind < 0)
            storage.recordReferenceField(receiver, field);
        else
            storage.recordSynchronizationField(receiver, field, static_cast<ReceiverSynchronizationKind>(rawKind));
        return 0;
    } catch (...) {
        construction.failure = std::current_exception();
        return 3;
    }
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_context_owner(void* context, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::ContextOwner, output,
        [](NativeReceiverStorage& storage) {
            return storage.contexts.retain(storage.contextScope().createContextOwner());
        });
}

extern "C" std::int32_t pipelang_native_v19_hosted_context_owner_call(
    void* context, std::uint64_t taggedHandle, std::int32_t operation) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(context);
    try {
        if ((taggedHandle & hostedSynchronizationTag) == 0)
            throw model::ExecutionFailure("native context owner is outside its owning binding");
        construction.storage.contexts.apply(taggedHandle & ~hostedSynchronizationTag,
                                            static_cast<ContextOwnerOperation>(operation));
        return 0;
    } catch (...) {
        construction.failure = std::current_exception();
        return 3;
    }
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_atomic(void* context, std::int32_t kind,
                                                                std::uint64_t initial, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::Atomic, output, [&](NativeReceiverStorage& storage) {
        return storage.atomics.create(static_cast<AtomicValueKind>(kind), initial);
    });
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_lock(void* context, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::Lock, output, [](NativeReceiverStorage& storage) {
        return storage.locks.create();
    });
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_semaphore(void* context, std::int64_t initial,
    std::int64_t maximum, std::int32_t hasMaximum, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::Semaphore, output, [&](NativeReceiverStorage& storage) {
        const auto semaphore = hasMaximum ? model::SemaphoreSlim::Create(initial, maximum)
                                          : model::SemaphoreSlim::Create(initial);
        return static_cast<std::uint64_t>(storage.semaphores.retain(semaphore));
    });
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_concurrent_sequence(
    void* context, std::int32_t kind, const OwnedShape* element, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::ConcurrentSequence, output, [&](NativeReceiverStorage& storage) {
        return storage.sequences.create(static_cast<ConcurrentSequenceKind>(kind), *element);
    });
}

extern "C" std::int32_t pipelang_native_v19_create_hosted_concurrent_dictionary(
    void* context, const OwnedShape* key, const OwnedShape* value, std::uint64_t* output) noexcept
{
    return createHostedSynchronization(context, ReceiverSynchronizationKind::ConcurrentDictionary, output, [&](NativeReceiverStorage& storage) {
        return storage.dictionaries.create(*key, *value);
    });
}

struct HostedInstance::State::Construction {
    struct PendingResource {
        HostedCleanup cleanup;
        void* receiver;
        std::shared_ptr<di::CleanupStatus> owner;
    };

    struct Reset {
        bool& active;
        ~Reset() { active = false; }
    };

    Construction(State& binding, model::CancellationToken cancellation, std::size_t steps)
        : state(binding), reset{binding.scopedConstructionActive}, cancellation(std::move(cancellation)),
          ownedInputs(binding.receiverStorage.referenceDomain()),
          execution{binding.receiverStorage, {}, ownedInputs, lists,
                    model::EvaluationBudget::forInitialization(model::defaultEvaluationSteps)}
    {
        state.scopedConstructionActive = true;
        requirePublication();
        execution.budget.setCancellationCheck([this] { return this->cancellation.is_cancelled(); });
        pendingResources.reserve(steps);
        pendingReceivers.reserve(steps);
        execution.transactions.beginConstruction(lists, {});
    }

    ~Construction()
    {
        if (published)
            return;
        execution.transactions.finishConstruction(false);
        for (auto* receiver : pendingReceivers)
            state.components.erase(receiver);
        for (const auto slot : pendingSlots)
            state.services[slot] = {};
    }

    bool unwind()
    {
        state.unwindingResources = true;
        Reset resetUnwind{state.unwindingResources};
        execution.budget = model::EvaluationBudget{};
        while (!pendingResources.empty()) {
            const auto& resource = pendingResources.back();
            auto outcome = model::Outcome::Failure;
            try {
                outcome = State::runResourceCleanup(execution, resource.cleanup, resource.receiver).outcome();
            } catch (const model::EvaluationLimitFailure&) {
                return false;
            } catch (const model::OperationCancelled&) {
                outcome = model::Outcome::Cancelled;
            } catch (...) {
                // Once entered, a callback cannot be replayed even when reporting failed.
            }
            if (resource.owner) {
                auto& status = *resource.owner;
                --status.pending;
                ++status.attempted;
                if (outcome == model::Outcome::Success) {
                    ++status.succeeded;
                } else {
                    ++status.unknown;
                    if (outcome == model::Outcome::Cancelled)
                        ++status.cancelled;
                    else
                        ++status.failed;
                }
            }
            pendingResources.pop_back();
        }
        return true;
    }

    void requirePublication() const
    {
        permit.requireCurrent();
        if (cancellation.is_cancelled())
            throw model::OperationCancelled("native scoped construction cancelled");
    }

    struct CreatedReceiver {
        void* receiver;
        const HostedFactoryVariant* variant;
    };

    model::Result<CreatedReceiver> createReceiver(
        const HostedConstructorRecipe& recipe, const std::uint64_t* const* inputs)
    {
        using Outcome = model::Result<CreatedReceiver>;
        execution.services = state.services;
        const auto index = static_cast<std::size_t>(&recipe - state.constructorRecipes.data());
        const auto factory = state.factories.empty() ? nullptr : state.factories[index];
        if (!factory) {
            auto* receiver = state.ownedReceivers.allocate(recipe.receiverWords, recipe.receiverTag);
            if (!receiver)
                return Outcome::Failure("native scoped receiver allocation failed");
            const auto status = recipe.construct(receiver, inputs, &execution, State::allocateReceiver);
            if (execution.failure)
                std::rethrow_exception(execution.failure);
            if (status != 0)
                return Outcome::Failure("native scoped source construction failed");
            return Outcome::Success({receiver, nullptr});
        }

        const auto mark = state.ownedReceivers.checkpoint();
        HostedFactoryResult result{};
        const auto status = factory(&result, inputs, &execution, State::allocateReceiver, &cancellation);
        if (execution.failure)
            std::rethrow_exception(execution.failure);
        if (status != 0)
            return Outcome::Failure("native scoped factory invocation failed");

        if (result.outcome.tag == ResultTag::Failure || result.outcome.tag == ResultTag::Cancelled) {
            const auto error = result.outcome.error;
            if (error.size > model::maximumStringBytes || (error.size != 0 && !error.bytes))
                return Outcome::Failure("native scoped factory returned an invalid error");
            const auto message = error.size == 0 ? std::string{} : std::string(error.bytes, error.size);
            if (result.outcome.tag == ResultTag::Cancelled)
                return Outcome::Cancelled(message);
            return Outcome::Failure(message);
        }

        if (result.outcome.tag != ResultTag::Success ||
            !state.ownedReceivers.allocatedSince(result.receiver, mark))
            return Outcome::Failure("native scoped factory must return a freshly allocated receiver");
        const auto* words = static_cast<const std::uint64_t*>(result.receiver);
        const auto tag = words[receiverTagWord];
        const auto width = words[receiverSizeWord];
        if (width == recipe.receiverWords && tag == recipe.receiverTag)
            return Outcome::Success({result.receiver, nullptr});

        if (!state.factoryVariants.empty()) {
            const auto& table = state.factoryVariants[index];
            const std::span variants(table.entries, table.count);
            const auto variant = std::find_if(variants.begin(), variants.end(), [&](const auto& entry) {
                return entry.receiverTag == tag && entry.receiverWords == width;
            });
            if (variant != variants.end())
                return Outcome::Success({result.receiver, &*variant});
        }
        return Outcome::Failure("native scoped factory returned the wrong source receiver");
    }

    HostedValueResult build(const HostedConstructorRecipe& recipe, std::span<const HostedConstructorArgument> arguments,
                            const std::shared_ptr<di::CleanupStatus>& cleanupOwner)
    {
        using Outcome = HostedValueResult;
        if (recipe.parameterCount != arguments.size())
            return Outcome::Failure("native hosted constructor argument count does not match");
        requirePublication();
        std::vector<std::size_t> slots;
        slots.reserve(arguments.size());
        std::array<std::vector<std::uint64_t>, maximumArguments> values;
        std::array<const std::uint64_t*, maximumArguments> inputs{};
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& parameter = recipe.parameters[index];
            if (parameter.service) {
                const auto* port = std::get_if<std::shared_ptr<HostedService>>(&arguments[index]);
                if (parameter.value || !port || !*port || (*port)->interfaceName() != parameter.service)
                    return Outcome::Failure("native scoped constructor received an incompatible host port");

                auto free = std::find_if(state.services.begin(), state.services.end(),
                    [](const auto& entry) { return !entry.service; });
                const auto slot = static_cast<std::size_t>(free - state.services.begin());
                if (free == state.services.end()) {
                    if (state.services.size() == model::ObjectDomain::maximumCells)
                        return Outcome::Failure("native scoped construction has exhausted its port storage");
                    state.services.push_back({});
                }
                if (state.nextScopedGeneration == std::numeric_limits<std::uint32_t>::max())
                    return Outcome::Failure("native scoped port identity exhausted");

                const auto generation = state.nextScopedGeneration++;
                pendingSlots.push_back(slot);
                state.services[slot] = {*port, generation, std::make_shared<const ScopedServiceUse>()};
                slots.push_back(slot);
                values[index].push_back((static_cast<std::uint64_t>(generation) << 32) | (slot + 1));
            } else {
                const auto* value = parameter.value;
                if (!value || value->words == 0 || value->words > maximumFrameWords || !retainedScopedInput(*value))
                    return Outcome::Failure("native scoped constructor requires an owned snapshot, same-graph object or borrowed host port");

                auto& words = values[index];
                words.resize(value->words);
                if (const auto* integer = std::get_if<std::int64_t>(&arguments[index]);
                    integer && value->kind == OwnedKind::Integer) {
                    words[0] = static_cast<std::uint64_t>(*integer);
                } else {
                    const auto* owned = std::get_if<HostedValue>(&arguments[index]);
                    if (!owned)
                        return Outcome::Failure("native scoped constructor received an incompatible owned argument");
                    ownedInputs.pack(*value, *owned, words.data(), &lists);
                }
            }
            inputs[index] = values[index].data();
        }


        const auto created = createReceiver(recipe, inputs.data());
        if (created.is_cancelled())
            return Outcome::Cancelled(created.error());
        if (created.is_failure())
            return Outcome::Failure(created.error());
        const auto& allocation = created.value();
        auto* receiver = allocation.receiver;
        pendingReceivers.push_back(receiver);
        if (const auto cleanup = state.cleanupFor(recipe, allocation.variant)) {
            pendingResources.push_back({cleanup, receiver, cleanupOwner});
            if (cleanupOwner)
                ++cleanupOwner->pending;
        }

        requirePublication();
        auto [component, inserted] = state.components.emplace(receiver, ComponentLease{
            {}, std::move(slots), std::make_shared<const ScopedServiceUse>(), &recipe, allocation.variant, {}});
        if (!inserted)
            return Outcome::Failure("native scoped receiver already has a construction lease");
        const auto word = reinterpret_cast<std::uint64_t>(receiver);
        HostedProjection projection(lists, state.receiverStorage.referenceDomain(), {}, &state.receiverStorage);
        auto exported = projection.copy(*recipe.type, &word);
        component->second.receiver = exported;
        return Outcome::Success(std::move(exported));
    }

    void publish()
    {
        requirePublication();
        state.receiverStorage.collectGraph(true);
        execution.transactions.finishConstruction(true);
        published = true;
    }

    State& state;
    Reset reset;
    model::CancellationToken cancellation;
    NativeLists lists;
    HostedInput ownedInputs;
    NativeHostedConstruction execution;
    model::InvocationPermit permit = model::currentPermit();
    std::vector<std::size_t> pendingSlots;
    std::vector<void*> pendingReceivers;
    std::vector<PendingResource> pendingResources;
    bool published = false;
};

model::Result<std::vector<HostedValue>> HostedInstance::constructServices(
    std::span<const HostedConstructionStep> steps) const
{
    return constructServices(steps, {});
}

model::Result<std::vector<HostedValue>> HostedInstance::constructServices(
    std::span<const HostedConstructionStep> steps, model::CancellationToken cancellation) const
{
    return constructServices(steps, std::move(cancellation), {});
}

model::Result<void> HostedInstance::requireIdleSourceSegment() const
{
    using Status = model::Result<void>;
    if (!state_ || state_->ownerThread != std::this_thread::get_id())
        return Status::Failure("native hosted operation requires its owning thread");
    if (state_->receiverStorage.blocksReentry() || state_->unwindingResources ||
        (state_->scopedConstructionActive && !state_->failedConstruction))
        return Status::Failure("native hosted model already has an active source segment");
    return Status::Success();
}

model::Result<void> HostedInstance::requireCompositionAdmission() const
{
    const auto idle = requireIdleSourceSegment();
    if (!idle.is_success())
        return idle;
    if (state_->failedConstruction)
        return model::Result<void>::Failure("di.construction_cleanup_pending");
    return model::Result<void>::Success();
}

model::Result<void> HostedInstance::settleFailedConstruction() const
{
    using Status = model::Result<void>;
    if (!state_ || !state_->failedConstruction)
        return Status::Success();
    if (state_->ownerThread != std::this_thread::get_id() || state_->receiverStorage.blocksReentry() || state_->unwindingResources)
        return Status::Failure("native scoped cleanup requires its owning source segment");
    if (!state_->failedConstruction->unwind())
        return Status::Failure("di.cleanup_pending");
    state_->failedConstruction.reset();
    return Status::Success();
}

model::Result<std::vector<HostedValue>> HostedInstance::constructServices(
    std::span<const HostedConstructionStep> steps, model::CancellationToken cancellation,
    std::span<const std::shared_ptr<di::CleanupStatus>> cleanupOwners) const
{
    using Outcome = model::Result<std::vector<HostedValue>>;
    const auto state = state_;
    if (!state || !state->receiverStorage.exposesObjectReferences())
        return Outcome::Failure("native scoped construction requires an exporting binding");
    if (state->ownerThread != std::this_thread::get_id())
        return Outcome::Failure("model accessed from a different owner thread");
    if (steps.empty() || steps.size() > model::ObjectDomain::maximumCells)
        return Outcome::Failure("native scoped construction has an invalid step count");
    if (!cleanupOwners.empty() && cleanupOwners.size() != steps.size())
        return Outcome::Failure("native scoped construction cleanup owners do not match steps");
    const auto settled = settleFailedConstruction();
    if (!settled.is_success())
        return Outcome::Failure(settled.error());
    if (state->scopedConstructionActive || state->receiverStorage.blocksReentry())
        return Outcome::Failure("native hosted model already has an active source segment");

    std::shared_ptr<State::Construction> construction;
    const auto outcome = [&]() -> Outcome {
        try {
            if (state->activeScope)
                state->activeScope->requireOwner();
            construction = std::make_shared<State::Construction>(*state, std::move(cancellation), steps.size());
            std::vector<HostedValue> receivers;
            receivers.reserve(steps.size());
            for (const auto& step : steps) {
                const auto recipe = std::find_if(state->constructorRecipes.begin(), state->constructorRecipes.end(),
                    [&](const auto& candidate) { return candidate.name == step.service; });
                if (recipe == state->constructorRecipes.end())
                    return Outcome::Failure("native hosted constructor recipe was not found");
                if (step.arguments.size() != recipe->parameterCount)
                    return Outcome::Failure("native hosted constructor argument count does not match");

                std::array<HostedConstructorArgument, maximumArguments> bindings;
                for (std::size_t index = 0; index < step.arguments.size(); ++index) {
                    if (const auto* literal = std::get_if<HostedConstructorArgument>(&step.arguments[index])) {
                        bindings[index] = *literal;
                    } else {
                        const auto dependency = std::get<HostedConstructionReference>(step.arguments[index]).priorStep;
                        if (dependency >= receivers.size())
                            return Outcome::Failure("native scoped construction dependency is unavailable");
                        bindings[index] = receivers[dependency];
                    }
                }
                const auto owner = cleanupOwners.empty() ? nullptr : cleanupOwners[receivers.size()];
                auto receiver = construction->build(*recipe, {bindings.data(), step.arguments.size()}, owner);
                if (receiver.is_cancelled())
                    return Outcome::Cancelled(receiver.error());
                if (receiver.is_failure())
                    return Outcome::Failure(receiver.error());
                receivers.push_back(std::move(receiver).value());
            }
            construction->publish();
            return Outcome::Success(std::move(receivers));
        } catch (const model::OperationCancelled& cancellation) {
            return Outcome::Cancelled(cancellation.what());
        } catch (const model::ExecutionFailure& failure) {
            return Outcome::Failure(failure.what());
        } catch (const std::bad_alloc&) {
            return Outcome::Failure("native scoped receiver allocation failed");
        }
    }();
    if (construction && !construction->published) {
        state->failedConstruction = std::move(construction);
        (void)settleFailedConstruction();
    }
    return outcome;
}

HostedValueResult HostedInstance::constructService(std::string_view service,
                                                     std::span<const HostedConstructorArgument> arguments) const
{
    if (arguments.size() > maximumArguments)
        return HostedValueResult::Failure("native hosted constructor argument count does not match");
    std::array<HostedConstructionInput, maximumArguments> inputs;
    std::copy(arguments.begin(), arguments.end(), inputs.begin());
    const HostedConstructionStep step{service, {inputs.data(), arguments.size()}};
    auto outcome = constructServices({&step, 1});
    if (outcome.is_cancelled())
        return HostedValueResult::Cancelled(outcome.error());
    if (outcome.is_failure())
        return HostedValueResult::Failure(outcome.error());
    return HostedValueResult::Success(std::move(outcome).value().front());
}

model::Result<void> HostedInstance::retireService(const HostedValue& receiver) const
{
    model::EvaluationBudget budget;
    std::optional<model::Result<void>> cleanup;
    const auto retired = retireService(receiver, budget, cleanup);
    return retired.is_success() ? cleanup.value_or(model::Result<void>::Success()) : retired;
}

model::Result<void> HostedInstance::retireService(const HostedValue& receiver, model::EvaluationBudget& budget,
                                                  std::optional<model::Result<void>>& cleanupOutcome) const
{
    using Status = model::Result<void>;
    if (!state_ || !state_->receiverStorage.exposesObjectReferences())
        return Status::Failure("native scoped port retirement requires an exporting binding");
    if (state_->ownerThread != std::this_thread::get_id())
        return Status::Failure("model accessed from a different owner thread");
    const auto settled = settleFailedConstruction();
    if (!settled.is_success())
        return settled;
    if (state_->scopedConstructionActive || state_->receiverStorage.blocksReentry())
        return Status::Failure("native hosted model already has an active source segment");

    state_->scopedConstructionActive = true;
    struct Reset {
        bool& active;
        ~Reset() { active = false; }
    } reset{state_->scopedConstructionActive};

    try {
        if (state_->activeScope)
            state_->activeScope->requireOwner();
        const auto port = std::find_if(state_->components.begin(), state_->components.end(), [&](const auto& entry) {
            return entry.second.receiver.sameObject(receiver);
        });
        if (port == state_->components.end())
            return Status::Failure("native hosted port has closed");

        if (port->second.methodUse.use_count() > 1)
            return Status::Failure("native scoped port has an active method");
        for (const auto slot : port->second.serviceSlots) {
            if (state_->services[slot].use.use_count() > 1)
                return Status::Failure("native scoped port has an active host call");
        }

        std::vector<HostServiceBinding> retired;
        retired.reserve(port->second.serviceSlots.size());
        const auto callback = state_->cleanupFor(*port->second.recipe, port->second.variant);
        if (callback && !port->second.cleanupOutcome) {
            NativeLists lists;
            HostedInput inputs(state_->receiverStorage.referenceDomain());
            NativeHostedConstruction execution{state_->receiverStorage, state_->services, inputs, lists, std::move(budget)};
            struct RestoreBudget {
                model::EvaluationBudget& original;
                NativeHostedConstruction& execution;
                ~RestoreBudget() { original = std::move(execution.budget); }
            } restore{budget, execution};
            execution.transactions.beginConstruction(lists, {});
            try {
                port->second.cleanupOutcome = State::runResourceCleanup(execution, callback, port->first);
            } catch (const model::EvaluationLimitFailure&) {
                execution.transactions.finishConstruction(false);
                return Status::Failure("di.cleanup_pending");
            } catch (...) {
                execution.transactions.finishConstruction(false);
                throw;
            }
            execution.transactions.finishConstruction(true);
        }
        cleanupOutcome = port->second.cleanupOutcome;
        for (const auto slot : port->second.serviceSlots)
            retired.push_back(std::exchange(state_->services[slot], {}));
        state_->components.erase(port);
        state_->receiverStorage.collectGraph(true);
        return Status::Success();
    } catch (const model::ExecutionFailure& failure) {
        return Status::Failure(failure.what());
    } catch (const std::bad_alloc&) {
        return Status::Failure("native scoped port retirement allocation failed");
    }
}

std::span<const HostedCompositionNode> HostedInstance::compositionNodes() const
{
    return state_ ? state_->composition : std::span<const HostedCompositionNode>{};
}

HostedValueResult HostedInstance::snapshot() const
{
    if (!state_ || !state_->snapshotShape)
        return HostedValueResult::Failure("native hosted object was built without a snapshot");
    if (state_->ownerThread != std::this_thread::get_id())
        return HostedValueResult::Failure("model accessed from a different owner thread");
    try {
        if (state_->activeScope)
            state_->activeScope->requireOwner();
        return HostedValueResult::Success(projectReceiverSnapshot(state_->receiverStorage, state_->receiver.data(),
                                                                 *state_->snapshotShape));
    } catch (const model::ExecutionFailure& failure) {
        return HostedValueResult::Failure(failure.what());
    }
}

model::Task<HostedResult> HostedInstance::start(model::TaskScope& scope, std::span<const HostedArgument> arguments,
                                               model::CancellationToken cancellation) const
{
    return startResult<std::int64_t>(scope, arguments, std::move(cancellation));
}

model::Task<HostedValueResult> HostedInstance::startValue(model::TaskScope& scope,
                                                         std::span<const HostedArgument> arguments,
                                                         model::CancellationToken cancellation) const
{
    return startResult<HostedValue>(scope, arguments, std::move(cancellation));
}

model::Task<HostedValueResult> HostedInstance::startWithValues(model::TaskScope& scope,
    std::span<const HostedCallArgument> arguments, model::CancellationToken cancellation) const
{
    return startResult<HostedValue>(scope, arguments, std::move(cancellation));
}

model::Task<HostedValueResult> HostedInstance::startMethod(model::TaskScope& scope, std::string_view name,
    std::span<const HostedCallArgument> arguments, model::CancellationToken cancellation) const
{
    if (const auto* method = state_ ? state_->findMethod(name) : nullptr)
        return startResult<HostedValue>(scope, arguments, std::move(cancellation), method);
    return scope.start<HostedValueResult>([](model::CancellationToken) -> model::Coroutine<HostedValueResult> {
        co_return HostedValueResult::Failure("native hosted method was not found");
    });
}

model::Task<HostedContextOwnerResult> HostedInstance::startContextOwner(
    model::TaskScope& scope, std::string_view name, model::CancellationToken cancellation) const
{
    const auto* method = state_ ? &state_->primary : nullptr;
    if (state_ && !name.empty())
        method = state_->findMethod(name);
    if (method)
        return startResult<std::shared_ptr<model::ContextOwner>>(scope, std::span<const HostedArgument>{},
                                                                 std::move(cancellation), method);
    return scope.start<HostedContextOwnerResult>([](model::CancellationToken) -> model::Coroutine<HostedContextOwnerResult> {
        co_return HostedContextOwnerResult::Failure("native hosted context getter was not found");
    });
}

template<class Value, class Input>
model::Task<model::Result<Value>> HostedInstance::startResult(model::TaskScope& scope,
                                                              std::span<const Input> arguments,
                                                              model::CancellationToken cancellation,
                                                              const HostedMethod* method) const
{
    using Result = model::Result<Value>;
    if (!method && state_)
        method = &state_->primary;
    if (!method || arguments.size() != method->entry->argumentCount) {
        return scope.start<Result>([](model::CancellationToken) -> model::Coroutine<Result> {
            co_return Result::Failure("native hosted invocation argument count or instance does not match");
        });
    }
    return scope.start<Result>(invocationFactory<Value>(scope, arguments, std::move(cancellation), *method));
}

template<class Value, class Input>
auto HostedInstance::invocationFactory(model::TaskScope& scope, std::span<const Input> arguments,
                                       model::CancellationToken cancellation, const HostedMethod& method,
                                       std::shared_ptr<void> receiver) const
{
    std::vector<HostedCallArgument> captured;
    captured.reserve(arguments.size());
    for (const auto& argument : arguments)
        captured.push_back(std::visit([](const auto& value) -> HostedCallArgument { return value; }, argument));

    return [&scope, state = state_, method = &method, inputs = std::move(captured),
            cancellation = std::move(cancellation), receiver = std::move(receiver)]
           (model::CancellationToken taskCancellation) mutable {
        return invoke<Value>(std::move(state), *method, scope, std::move(inputs),
                             model::CancellationToken::combine(std::move(cancellation), std::move(taskCancellation)),
                             std::move(receiver));
    };
}

model::Result<model::Task<HostedValueResult>> HostedInstance::tryStartMethod(
    model::TaskScope& scope, const std::shared_ptr<model::TaskGroup>& group, std::string_view name,
    std::span<const HostedCallArgument> arguments, model::CancellationToken cancellation) const
{
    using Admission = model::Result<model::Task<HostedValueResult>>;
    if (state_ && state_->ownerThread != std::this_thread::get_id())
        return Admission::Failure("model accessed from a different owner thread");
    const auto* method = state_ ? state_->findMethod(name) : nullptr;
    if (!method)
        return Admission::Failure("native hosted method was not found");
    if (arguments.size() != method->entry->argumentCount)
        return Admission::Failure("native hosted invocation argument count or instance does not match");
    if (cancellation.is_cancelled())
        return Admission::Cancelled("native hosted invocation cancelled before admission");
    try {
        return scope.tryStart<HostedValueResult>(group,
            invocationFactory<HostedValue>(scope, arguments, std::move(cancellation), *method));
    } catch (const model::ExecutionFailure& failure) {
        return Admission::Failure(failure.what());
    }
}

model::Result<model::Task<HostedValueResult>> HostedInstance::tryStartComponentMethod(
    model::TaskScope& scope, const std::shared_ptr<model::TaskGroup>& group, std::size_t node,
    const HostedValue& receiver, std::string_view name, std::span<const HostedCallArgument> arguments,
    model::CancellationToken cancellation) const
{
    using Admission = model::Result<model::Task<HostedValueResult>>;
    if (!state_ || state_->ownerThread != std::this_thread::get_id())
        return Admission::Failure("native service method requires its owning binding thread");
    if (node >= state_->composition.size() || state_->composition[node].recipe == 0)
        return Admission::Failure("native hosted service method was not found");
    const auto& service = state_->composition[node];
    const auto* recipe = &state_->constructorRecipes[service.recipe - 1];
    const auto component = std::find_if(state_->components.begin(), state_->components.end(), [&](const auto& entry) {
        return entry.second.recipe == recipe && entry.second.receiver.sameObject(receiver);
    });
    if (component == state_->components.end())
        return Admission::Failure("native hosted port has closed");

    const HostedCompositionMethods* table = nullptr;
    if (component->second.variant)
        table = &component->second.variant->methods;
    else if (node < state_->compositionMethods.size())
        table = &state_->compositionMethods[node];
    if (!table)
        return Admission::Failure("native hosted service method was not found");

    const std::span methods(table->methods, table->methodCount);
    const auto method = std::find_if(methods.begin(), methods.end(),
        [&](const auto& candidate) { return name == candidate.name; });
    if (method == methods.end())
        return Admission::Failure("native hosted service method was not found");
    if (arguments.size() != method->entry->argumentCount)
        return Admission::Failure("native hosted invocation argument count does not match");
    if (cancellation.is_cancelled())
        return Admission::Cancelled("native hosted invocation cancelled before admission");

    // The alias retains the construction lease even when the task is parked.
    auto hold = std::shared_ptr<void>(component->second.methodUse, component->first);
    return scope.tryStart<HostedValueResult>(group,
        invocationFactory<HostedValue>(scope, arguments, std::move(cancellation), *method, std::move(hold)));
}

template<class Value>
model::Coroutine<model::Result<Value>> HostedInstance::invoke(std::shared_ptr<State> state, const HostedMethod& method,
                                                             model::TaskScope& scope,
                                                             std::vector<HostedCallArgument> arguments,
                                                             model::CancellationToken cancellation,
                                                             std::shared_ptr<void> receiver)
{
    using Result = model::Result<Value>;
    constexpr bool contextGetter = std::is_same_v<Value, std::shared_ptr<model::ContextOwner>>;
    if (state->ownerThread != std::this_thread::get_id())
        co_return Result::Failure(contextGetter ? "context.wrong_owner_thread" :
                                                  "model accessed from a different owner thread");
    if (state->scopedConstructionActive)
        co_return Result::Failure("native hosted model already has an active source segment");
    const auto& entry = *method.entry;
    const bool multipleTasks = state->receiverStorage.permitsMultipleTasks;
    if constexpr (std::is_same_v<Value, std::int64_t>) {
        if (!method.successShape || method.successShape->kind != OwnedKind::Integer)
            co_return Result::Failure("native hosted entry does not return an integer");
    } else if constexpr (contextGetter) {
        if (!method.successShape || method.successShape->kind != OwnedKind::ContextOwnerReference)
            co_return Result::Failure("native hosted entry does not return a context owner");
    } else if (method.successShape && method.successShape->kind == OwnedKind::ContextOwnerReference) {
        co_return Result::Failure("use startContextOwner to read a native context owner");
    }
    std::array<Argument, maximumArguments> inputs{};
    std::array<InvocationValue, maximumArguments> values{};
    const auto cancellationArguments = cancellationArgumentMask(entry);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto mask = std::uint64_t{1} << index;
        const bool requiresToken = (cancellationArguments & mask) != 0;
        const auto* token = std::get_if<model::CancellationToken>(&arguments[index]);
        if (requiresToken != (token != nullptr))
            co_return Result::Failure("native hosted invocation argument type does not match");
        if (token) {
            inputs[index].cancellation = token;
            continue;
        }
        const auto* shape = method.argumentShapes ? method.argumentShapes[index] : nullptr;
        if (const auto* integer = std::get_if<std::int64_t>(&arguments[index]);
            integer && (!shape || shape->kind == OwnedKind::Integer)) {
            inputs[index].integer = *integer;
            continue;
        }
        const auto* owned = std::get_if<HostedValue>(&arguments[index]);
        if (!shape || !owned)
            co_return Result::Failure("native hosted invocation argument type does not match");
        values[index] = {shape, owned, (method.indirectArguments & mask) != 0};
    }

    if (state->running.test_and_set(std::memory_order_acquire)) {
        if (!multipleTasks || state->receiverStorage.blocksReentry())
            co_return Result::Failure("native hosted model already has an active invocation");
        if (state->activeScope != &scope)
            co_return Result::Failure("native hosted tasks must share an owning task scope");
    }
    ++state->activeInvocations;
    struct InvocationLease {
        State& state;
        bool collected = false;

        void collect()
        {
            state.receiverStorage.collectGraph(true);
            collected = true;
        }

        ~InvocationLease()
        {
            if (!collected) {
                try {
                    collect();
                } catch (...) {
                    // Preserve an exception already leaving the invocation.
                }
            }
            if (--state.activeInvocations == 0) {
                state.activeScope = nullptr;
                state.running.clear(std::memory_order_release);
            }
        }
    } lease{*state};
    state->activeScope = &scope;

    std::uint64_t waits = 0;
    auto* target = receiver ? receiver.get() : state->receiver.data();
    const auto invocationValues = method.argumentShapes
        ? std::span<const InvocationValue>(values.data(), arguments.size()) : std::span<const InvocationValue>{};
    struct Output {
        const OwnedShape* shape;
        NativeReceiverStorage& storage;
        Value value;
    } output{method.successShape, state->receiverStorage, {}};

    const auto project = [](void* context, const std::uint64_t* payload, const NativeLists& lists) {
        auto& projected = *static_cast<Output*>(context);
        if constexpr (std::is_same_v<Value, std::int64_t>) {
            std::memcpy(&projected.value, payload, sizeof(projected.value));
        } else if constexpr (contextGetter) {
            const auto handle = *payload;
            if ((handle & hostedSynchronizationTag) == 0)
                throw model::ExecutionFailure("native context owner is outside its owning binding");
            projected.value = projected.storage.contexts.at(handle & ~hostedSynchronizationTag);
            projected.value->requireOwner();
        } else if (projected.shape) {
            const bool exports = projected.storage.exposesObjectReferences();
            auto* exporter = exports ? &projected.storage : nullptr;
            const auto* domain = exports ? projected.storage.referenceDomain() : nullptr;
            projected.value = HostedProjection(lists, domain, {}, exporter).copy(*projected.shape, payload);
        }
    };
    const auto completed = co_await runNative(scope, entry, target, inputs.data(),
                                            std::move(cancellation), waits, project, &output, state->services,
                                            &state->receiverStorage, invocationValues);
    lease.collect();
    if (completed.is_cancelled())
        co_return Result::Cancelled(completed.error());
    if (completed.is_failure())
        co_return Result::Failure(completed.error());
    co_return Result::Success(std::move(output.value));
}

}
