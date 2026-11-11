// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionFactoryProfile.hpp"
#include "ResourceBudget.hpp"

#include <algorithm>

namespace pipelang::models {
namespace {
[[noreturn]] void refuse(const CompositionOrigin& origin, const std::string& message) {
    Diagnostic diagnostic("composition factory profile: " + message, origin.offset);
    diagnostic.module = origin.module;
    diagnostic.path = origin.path;
    throw diagnostic;
}
}

CompositionFactoryProfile::CompositionFactoryProfile(const Program& program,
        const std::vector<CompositionGraph>& graphs) : program_(program), graphs_(graphs) {
    for (const auto& declaration : program.declarations) {
        declarations_.emplace(declaration.name, &declaration);
        for (const auto& method : declaration.methods)
            methods_.emplace(method.identity, CompositionMethod{&declaration, &method});
    }
}

const Declaration* CompositionFactoryProfile::declaration(const Type& type) const {
    const auto found = declarations_.find(type.name);
    return found == declarations_.end() ? nullptr : found->second;
}

const Method& CompositionFactoryProfile::constructor(const std::string& identity) const {
    const auto found = methods_.find(identity);
    if (found == methods_.end() || !found->second.method->constructor)
        throw Diagnostic("composition factory lost its checked constructor identity", 0);
    return *found->second.method;
}

CompositionMethod CompositionFactoryProfile::checkedMethod(const std::string& identity) const {
    const auto found = methods_.find(identity);
    if (found == methods_.end())
        throw Diagnostic("composition lost its checked method identity", 0);
    return found->second;
}

void CompositionFactoryProfile::admitFactoryCleanupSubtypes(const CompositionGraph& graph,
        const CompositionService& service) {
    for (const auto& candidate : program_.declarations) {
        work(service.origin);
        if (candidate.kind != "Class" || candidate.modelExported ||
            (candidate.name != service.implementation.name &&
                std::find(candidate.ancestors.begin(), candidate.ancestors.end(), service.implementation.name) == candidate.ancestors.end()))
            continue;
        std::optional<std::size_t> applicable;
        for (std::size_t index = 0; index < graph.cleanupContracts.size(); ++index) {
            work(service.origin);
            const auto& contract = graph.cleanupContracts[index].contract;
            if (std::none_of(candidate.interfaces.begin(), candidate.interfaces.end(),
                    [&](const auto& implemented) { return implemented == contract; }))
                continue;
            if (applicable)
                refuse(service.origin, "factory return subtype '" + candidate.sourceName +
                    "' matches multiple selected cleanup contracts; return an unambiguous cleanup-declaring concrete type");
            applicable = index;
        }
        if (applicable != service.cleanupIndex)
            refuse(service.origin, "factory return subtype '" + candidate.sourceName +
                "' has cleanup not exposed by declared return class '" + service.implementation.displayText() +
                "'; return a concrete cleanup-declaring type or cleanup-declaring base");
    }
}

void CompositionFactoryProfile::inspectFactory(const std::string& identity, const CompositionOrigin& initialOrigin) {
    std::vector<std::string> pending{identity};
    std::set<std::string> visited;
    while (!pending.empty()) {
        const auto next = std::move(pending.back());
        pending.pop_back();
        work(initialOrigin);
        if (!visited.insert(next).second)
            continue;
        const auto entry = checkedMethod(next);
        const auto& method = *entry.method;
        const CompositionOrigin methodOrigin{entry.owner->module, entry.owner->originPath, method.offset};
        if (method.asynchronous || !method.parallelCalls.empty())
            refuse(methodOrigin, "resource factories and their helpers must be synchronous");
        pending.insert(pending.end(), method.calls.begin(), method.calls.end());
        pending.insert(pending.end(), method.dispatchTargets.begin(), method.dispatchTargets.end());
        const auto fieldRoot = [](Expr target) {
            while (target && target->kind == "member" && target->resolution == "record")
                target = target->children[0];
            return target && target->resolution == "object_field" ? target : Expr{};
        };
        const auto admitWrite = [&](const Expr& target) {
            const auto field = fieldRoot(target);
            if (!field)
                return;
            const bool ownInitialization = method.constructor && field->owner == entry.owner->name &&
                (field->kind == "name" || (field->kind == "member" && !field->children.empty() &&
                    field->children[0]->kind == "this"));
            if (!ownInitialization) {
                auto origin = methodOrigin;
                origin.offset = field->offset;
                refuse(origin, "resource factories cannot mutate existing source-object fields; initialize only this object's fields inside its constructor");
            }
        };
        std::vector<Expr> expressions{method.baseConstructor};
        expressions.insert(expressions.end(), method.fieldInitializers.begin(), method.fieldInitializers.end());
        std::vector<const Statement*> statements;
        for (const auto& statement : method.body)
            statements.push_back(&statement);
        while (!statements.empty()) {
            const auto* statement = statements.back();
            statements.pop_back();
            work(methodOrigin);
            if (statement->kind == "lock" || statement->kind == "parallel_foreach")
                refuse(methodOrigin, "resource factory synchronization/parallel work is not admitted");
            if (statement->kind == "assign")
                admitWrite(statement->target);
            expressions.push_back(statement->expression);
            expressions.push_back(statement->target);
            for (const auto* branch : {&statement->initialization, &statement->body, &statement->iteration,
                                      &statement->alternative, &statement->cleanup}) {
                for (const auto& child : *branch)
                    statements.push_back(&child);
            }
        }
        while (!expressions.empty()) {
            const auto expression = expressions.back();
            expressions.pop_back();
            if (!expression)
                continue;
            work(methodOrigin);
            const auto& effect = expression->resolution;
            const bool atomicMutation = effect.starts_with("atomic_") &&
                effect != "atomic_construct" && effect != "atomic_load";
            if (atomicMutation || effect.starts_with("concurrent_mutate_") ||
                effect == "sync_release" || effect.starts_with("sync_wait")) {
                auto origin = methodOrigin;
                origin.offset = expression->offset;
                refuse(origin, "resource factory nontransactional shared-handle mutation/wait/release is not admitted");
            }
            if (expression->kind == "await" || expression->resolution.ends_with("_async"))
                refuse(methodOrigin, "resource factories and their helpers must be synchronous");
            if ((expression->resolution == "list_add" || expression->resolution.starts_with("collection_mutate_")) &&
                !expression->children.empty() && !expression->children[0]->children.empty())
                admitWrite(expression->children[0]->children[0]);
            pending.insert(pending.end(), expression->targets.begin(), expression->targets.end());
            if (expression->targets.empty() && !expression->callable.empty() && methods_.contains(expression->callable))
                pending.push_back(expression->callable);
            expressions.insert(expressions.end(), expression->children.begin(), expression->children.end());
        }
    }
}

bool CompositionFactoryProfile::ownedValue(const Type& type) const {
    static const std::set<std::string> scalars{
        "void", "bool", "int", "uint32", "float", "float32", "string", "ByteBuffer", "Error"};
    if (scalars.contains(type.name))
        return type.arguments.empty();
    const auto* owner = declaration(type);
    if (owner)
        return (owner->kind == "Record" || owner->kind == "Enum") && type.arguments.empty();
    static const std::set<std::string> values{
        "Optional", "Result", "List", "Array", "Dictionary", "Queue", "Stack", "KeyValuePair"};
    return values.contains(type.name) && !type.arguments.empty() &&
        std::all_of(type.arguments.begin(), type.arguments.end(),
            [&](const Type& argument) { return ownedValue(argument); });
}

void CompositionFactoryProfile::work(const CompositionOrigin& origin) {
    if (budgetExceeded(work_, 1, resourceBudget().tokens))
        refuse(origin, "admission work exceeds tokens; configure tokens");
    ++work_;
}

const std::vector<CompositionMethod>& CompositionFactoryProfile::methods(const Type& type) {
    if (const auto found = exportedMethods_.find(type.name); found != exportedMethods_.end())
        return found->second;
    std::vector<CompositionMethod> result;
    std::set<std::string> signatures;
    const auto* owner = declaration(type);
    while (owner) {
        for (const auto& method : owner->methods) {
            if (method.constructor || !method.visible || method.protectedMember)
                continue;
            std::string signature = method.name;
            for (const auto& parameter : method.parameters)
                signature += ':' + parameter.type.text();
            if (signatures.insert(signature).second)
                result.push_back({owner, &method});
        }
        const auto found = declarations_.find(owner->baseClass);
        owner = found == declarations_.end() ? nullptr : found->second;
    }
    return exportedMethods_.emplace(type.name, std::move(result)).first->second;
}

void CompositionFactoryProfile::queue(const std::string& identity, const CompositionOrigin& origin) {
    work(origin);
    if (!admittedMethods_.insert(identity).second)
        return;
    const auto found = methods_.find(identity);
    if (found == methods_.end())
        refuse(origin, "unresolved checked call target '" + identity + "'");
    const auto& entry = found->second;
    pending_.push_back({entry.method, {entry.owner->module, entry.owner->originPath, entry.method->offset}});
}

void CompositionFactoryProfile::inspectHost(const Expr& expression, const CompositionOrigin& origin) {
    if (expression->children.empty() || expression->children[0]->children.empty())
        refuse(origin, "host call lacks its checked receiver");
    const auto& target = expression->children[0];
    const auto* host = declaration(target->children[0]->type);
    if (!host || host->kind != "Interface" || host->sourceInterface)
        refuse(origin, "host call lacks a checked host interface");
    const Method* selected = nullptr;
    for (const auto& candidate : host->methods) {
        if (candidate.name != target->text || candidate.parameters.size() + 1 != expression->children.size())
            continue;
        bool matches = true;
        for (std::size_t index = 0; index < candidate.parameters.size(); ++index) {
            const auto& argument = expression->children[index + 1];
            const auto& type = argument->convertTo.name.empty() ? argument->type : argument->convertTo;
            matches = matches && type == candidate.parameters[index].type;
        }
        if (matches) {
            if (selected)
                refuse(origin, "ambiguous checked host signature");
            selected = &candidate;
        }
    }
    if (!selected || !selected->pure || selected->asynchronous || !ownedValue(selected->result) ||
        !std::all_of(selected->parameters.begin(), selected->parameters.end(),
            [&](const Field& parameter) { return ownedValue(parameter.type); }))
        refuse(origin, "constructors require transitive pure owned host reads; effectful host acquisition needs explicit cleanup/factory integration");
}

void CompositionFactoryProfile::inspectExpression(const Expr& expression, const CompositionOrigin& methodOrigin) {
    std::vector<Expr> pending{expression};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!current)
            continue;
        auto origin = methodOrigin;
        origin.offset = current->offset;
        work(origin);
        const auto& effect = current->resolution;
        const bool atomicMutation = effect.starts_with("atomic_") &&
            effect != "atomic_construct" && effect != "atomic_load";
        if (atomicMutation || effect.starts_with("concurrent_mutate_") ||
            effect == "sync_release" || effect.starts_with("sync_wait"))
            refuse(origin, "constructor nontransactional shared-handle mutation/wait/release requires a later lifecycle profile");
        if (current->resolution == "host" || current->resolution == "host_sync" ||
            current->resolution == "host_sync_value")
            inspectHost(current, origin);
        if (current->resolution == "local_async" || current->resolution == "object_async" ||
            current->resolution == "source_interface_async" || current->resolution == "class_virtual_async" ||
            current->kind == "await")
            refuse(origin, "constructor async work requires the later lifecycle profile");
        for (const auto& target : current->targets)
            queue(target, origin);
        if (current->targets.empty() && !current->callable.empty() && methods_.contains(current->callable))
            queue(current->callable, origin);
        pending.insert(pending.end(), current->children.begin(), current->children.end());
    }
}

void CompositionFactoryProfile::inspectStatements(const std::vector<Statement>& statements,
        const CompositionOrigin& origin) {
    std::vector<const Statement*> pending;
    for (const auto& statement : statements)
        pending.push_back(&statement);
    while (!pending.empty()) {
        const auto* statement = pending.back();
        pending.pop_back();
        work(origin);
        if (statement->kind == "lock" || statement->kind == "parallel_foreach") {
            auto statementOrigin = origin;
            statementOrigin.offset = statement->offset;
            refuse(statementOrigin, "constructor synchronization/parallel work requires a later lifecycle profile");
        }
        inspectExpression(statement->expression, origin);
        inspectExpression(statement->target, origin);
        for (const auto* branch : {&statement->initialization, &statement->body, &statement->iteration,
                                  &statement->alternative, &statement->cleanup}) {
            for (const auto& child : *branch)
                pending.push_back(&child);
        }
    }
}

void CompositionFactoryProfile::inspectMethod(const Method& method, const CompositionOrigin& origin) {
    if (method.asynchronous || !method.parallelCalls.empty())
        refuse(origin, "constructor async/parallel work requires the later lifecycle profile");
    for (const auto& identity : method.calls)
        queue(identity, origin);
    inspectExpression(method.baseConstructor, origin);
    for (const auto& initializer : method.fieldInitializers)
        inspectExpression(initializer, origin);
    inspectStatements(method.body, origin);
}

void CompositionFactoryProfile::admit() {
    if (!program_.projectEnabled || graphs_.empty())
        throw Diagnostic("composition factory emission requires project source compositions", 0);
    for (const auto& graph : graphs_) {
        for (const auto& cleanup : graph.cleanupContracts) {
            if (cleanup.method == "SourceMethods")
                refuse(cleanup.selectorOrigin, "cleanup method name conflicts with the generated checked bridge type");
        }
        const auto compositionName = graph.name.substr(graph.name.rfind('.') + 1);
        static const std::set<std::string> facadeNames{
            "Root", "Scope", "Create", "RootInputs", "ScopeInputs", "RootCache", "ScopeCache", "Retained", "Stage"};
        if (facadeNames.contains(compositionName))
            refuse(graph.origin, "composition name '" + compositionName + "' conflicts with a native facade type/member; choose another composition name");
        std::set<std::string> exportNames;
        for (const auto& service : graph.services) {
            work(service.origin);
            if (service.factory && service.factory->method == "SourceMethods")
                refuse(service.factory->origin, "factory method name conflicts with the generated checked bridge type");
            if (service.input == CompositionInput::Borrowed) {
                const auto* owner = declaration(service.service);
                if (!owner || owner->kind != "Interface" || owner->sourceInterface)
                    refuse(service.origin, "borrowed source references require an owner-admission profile; only host interface inputs are supported here");
            }
            if (service.factory) {
                admitFactoryCleanupSubtypes(graph, service);
                inspectFactory(service.factory->identity, service.factory->origin);
            } else if (service.input == CompositionInput::None) {
                queue(service.constructorIdentity, service.constructorOrigin);
            }
        }
        for (const auto index : graph.exports) {
            const auto& service = graph.services.at(index);
            const auto* owner = declaration(service.service);
            if (!owner || (owner->kind != "Class" && !owner->sourceInterface))
                refuse(service.origin, "typed facade exports require source classes or source interfaces");
            const auto separator = owner->sourceName.rfind('.');
            const auto name = owner->sourceName.substr(separator == std::string::npos ? 0 : separator + 1);
            if (name + "Handle" == compositionName)
                refuse(service.origin, "export handle name conflicts with the composition type; choose another composition name");
            if (!exportNames.insert(name).second)
                refuse(service.origin, "duplicate exported basename '" + name + "'; use separate compositions until qualified/aliased facade accessors are supported");
            std::set<std::string> sourceSignatures;
            std::set<std::string> boundarySignatures;
            for (const auto& entry : methods(service.service)) {
                const auto& method = *entry.method;
                const CompositionOrigin origin{entry.owner->module, entry.owner->originPath, method.offset};
                std::string signature = method.name;
                for (const auto& parameter : method.parameters)
                    signature += ':' + parameter.type.text();
                const auto boundarySignature = signature + ":CancellationToken";
                if (boundarySignatures.contains(signature) || sourceSignatures.contains(boundarySignature))
                    refuse(origin, "source overloads overlap the explicit facade cancellation boundary; use distinct method names in this emission profile");
                sourceSignatures.insert(signature);
                boundarySignatures.insert(boundarySignature);
                if (method.name == name + "Handle" || method.name == "SourceMethods")
                    refuse(origin, "exported method name conflicts with a generated handle/bridge type in this native facade profile");
                const auto& returned = method.asynchronous ? method.result.arguments.at(0) : method.result;
                if (!method.typeParameters.empty() ||
                    (!method.sourceName.empty() && method.sourceName != method.name) ||
                    !ownedValue(returned) || !std::all_of(method.parameters.begin(), method.parameters.end(),
                        [&](const Field& parameter) {
                            return ownedValue(parameter.type) ||
                                (parameter.type.name == "CancellationToken" && parameter.type.arguments.empty());
                        }))
                    refuse(origin, "exported facade methods require nongeneric owned-value signatures");
                if (returned.name == "Result" && returned.arguments.at(1).name != "string")
                    refuse(origin, "typed nonstring Result errors require a later facade profile");
            }
        }
    }
    while (!pending_.empty()) {
        const auto entry = pending_.back();
        pending_.pop_back();
        inspectMethod(*entry.first, entry.second);
    }
}

}
