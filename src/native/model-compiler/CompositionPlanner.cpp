// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionPlanner.hpp"
#include "ResourceBudget.hpp"
#include "syntax.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace pipelang::models {
namespace {

CompositionOrigin originOf(const CompositionDeclaration& composition, std::size_t offset) {
    return {composition.module, composition.originPath, offset};
}

[[noreturn]] void refuse(const CompositionOrigin& origin, const std::string& message) {
    Diagnostic diagnostic(message, origin.offset);
    diagnostic.module = origin.module;
    diagnostic.path = origin.path;
    throw diagnostic;
}

std::string identityOf(const CompositionDeclaration& composition) {
    if (!composition.sourceName.empty())
        return composition.sourceName;
    return composition.name;
}

class Planner {
public:
    explicit Planner(const Program& program) : program_(program) {
        for (const auto& declaration : program.declarations)
            types_.emplace(declaration.name, &declaration);
        for (const auto& composition : program.compositions) {
            const auto identity = identityOf(composition);
            if (identity.empty() || !compositions_.emplace(identity, &composition).second)
                refuse(originOf(composition, composition.offset), "duplicate or empty composition identity '" + identity + "'");
        }
    }

    std::vector<CompositionGraph> run() {
        std::vector<CompositionGraph> graphs;
        for (const auto& [identity, composition] : compositions_) {
            if (!program_.projectEnabled)
                refuse(originOf(*composition, composition->offset), "composition declarations require project mode");
            graphs.push_back(plan(*composition, identity));
        }
        return graphs;
    }

private:
    struct Registration {
        const CompositionDeclaration* composition;
        const CompositionRegistration* syntax;
        Type service;
        Type implementation;
        const Declaration* factoryOwner = nullptr;
        const Method* factoryMethod = nullptr;
        Type factoryReceiver;
    };

    void work(const CompositionOrigin& origin) {
        if (budgetExceeded(work_, 1, resourceBudget().tokens))
            refuse(origin, "composition graph work resource budget exceeded; configure tokens");
        ++work_;
    }

    void bytes(std::size_t additional, const CompositionOrigin& origin) {
        if (budgetExceeded(metadataBytes_, additional, resourceBudget().outputBytes))
            refuse(origin, "composition graph metadata resource budget exceeded; configure output_bytes");
        metadataBytes_ += additional;
    }

    void claimOrigin(const CompositionOrigin& origin) {
        bytes(origin.module.size(), origin);
        bytes(origin.path.size(), origin);
    }

    const Declaration* declaration(const Type& type) const {
        const auto found = types_.find(type.name);
        return found == types_.end() ? nullptr : found->second;
    }

    Type sourceType(Type type, const CompositionOrigin& origin) const {
        if (!type.arguments.empty())
            refuse(origin, "composition bindings require nongeneric types in this checkpoint");
        const auto* owner = declaration(type);
        if (!owner || !owner->modelExported)
            return type;
        if (owner->sourceObjectFacet.empty())
            refuse(origin, "composition cannot inject an independent native model owner without a constructible source facet: '" + type.displayText() + "'");
        const auto facet = types_.find(owner->sourceObjectFacet);
        if (facet == types_.end() || facet->second->modelExported || !facet->second->generatedObjectFacet)
            refuse(origin, "composition model source facet is unavailable: '" + type.displayText() + "'");
        type.name = facet->second->name;
        if (type.sourceName.empty())
            type.sourceName = owner->sourceName;
        return type;
    }

    CompositionInput inputKind(const Type& type, const CompositionOrigin& origin) const {
        static const std::set<std::string> primitives{
            "bool", "int", "uint32", "float", "float32", "string", "ByteBuffer"};
        if (primitives.contains(type.name))
            return CompositionInput::Snapshot;
        const auto* owner = declaration(type);
        if (!owner || !owner->typeParameters.empty() || !owner->genericArguments.empty())
            refuse(origin, "unsupported composition input type '" + type.displayText() + "'; use a plain value snapshot or object/interface input");
        if (owner->kind == "Record" || owner->kind == "Enum")
            return CompositionInput::Snapshot;
        if (owner->kind == "Class" && !owner->modelExported)
            return CompositionInput::Borrowed;
        if (owner->kind == "Interface")
            return CompositionInput::Borrowed;
        refuse(origin, "composition input cannot retain type '" + type.displayText() + "'");
    }

    bool compatible(const Declaration& implementation, const Type& service) const {
        if (implementation.name == service.name)
            return true;
        const auto* contract = declaration(service);
        if (!contract)
            return false;
        if (contract->kind == "Class") {
            return std::find(implementation.ancestors.begin(), implementation.ancestors.end(), service.name) !=
                implementation.ancestors.end();
        }
        return contract->sourceInterface && std::any_of(implementation.interfaces.begin(), implementation.interfaces.end(),
            [&](const Type& implemented) { return implemented == service; });
    }

    const Method& constructor(const Declaration& implementation, const CompositionOrigin& origin) const {
        const Method* selected = nullptr;
        for (const auto& method : implementation.methods) {
            if (!method.constructor || !method.visible || method.protectedMember)
                continue;
            if (selected)
                refuse(origin, "ambiguous public constructors for composition implementation '" + implementation.sourceName + "'; explicit constructor selection is not supported in this checkpoint");
            selected = &method;
        }
        if (!selected)
            refuse(origin, "composition implementation has no accessible public constructor: '" + implementation.sourceName + "'");
        if (selected->asynchronous || !selected->typeParameters.empty() || selected->identity.empty())
            refuse(origin, "composition requires a checked synchronous nongeneric constructor");
        return *selected;
    }

    std::vector<const CompositionDeclaration*> fragments(const CompositionDeclaration& root) {
        struct Frame {
            const CompositionDeclaration* composition;
            std::vector<const CompositionInclude*> includes;
            std::size_t next = 0;
        };
        std::map<std::string, int> states;
        std::vector<Frame> stack;
        std::vector<const CompositionDeclaration*> result;
        const auto push = [&](const CompositionDeclaration& composition) {
            Frame frame{&composition, {}, 0};
            std::set<std::string> included;
            for (const auto& include : composition.includes) {
                const auto origin = originOf(composition, include.offset);
                work(origin);
                if (!included.insert(include.name).second)
                    refuse(origin, "duplicate direct composition include '" + include.name + "'");
                frame.includes.push_back(&include);
            }
            std::sort(frame.includes.begin(), frame.includes.end(), [](const auto* left, const auto* right) {
                return left->name < right->name;
            });
            states[identityOf(composition)] = 1;
            stack.push_back(std::move(frame));
        };
        push(root);
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == frame.includes.size()) {
                states[identityOf(*frame.composition)] = 2;
                result.push_back(frame.composition);
                stack.pop_back();
                continue;
            }
            const auto& include = *frame.includes[frame.next++];
            const auto origin = originOf(*frame.composition, include.offset);
            const auto target = compositions_.find(include.name);
            if (target == compositions_.end())
                refuse(origin, "included composition was not supplied: '" + include.name + "'");
            if (states[include.name] == 1)
                refuse(origin, "composition include cycle through '" + include.name + "'");
            if (states[include.name] != 2)
                push(*target->second);
        }
        return result;
    }

    std::string signature(const Declaration& owner, const Method& method) const {
        std::string result = owner.sourceName;
        if (result.empty())
            result = owner.name;
        if (!method.constructor)
            result += '.' + method.name;
        result += '(';
        bool first = true;
        for (const auto& parameter : method.parameters) {
            if (!first)
                result += ", ";
            result += parameter.type.displayText();
            first = false;
        }
        return result + ')';
    }

    std::pair<const Declaration*, const Method*> selectedMethod(const Type& type,
            const std::string& name, const CompositionOrigin& origin) const {
        const Declaration* selectedOwner = nullptr;
        const Method* selected = nullptr;
        std::set<std::string> signatures;
        auto* owner = declaration(type);
        if (!owner || (owner->kind != "Class" && !owner->sourceInterface))
            refuse(origin, "composition method selectors require a source class or source interface");
        while (owner) {
            for (const auto& method : owner->methods) {
                if (method.constructor || method.staticMethod || method.name != name)
                    continue;
                std::string key;
                for (const auto& parameter : method.parameters)
                    key += ':' + parameter.type.text();
                if (!signatures.insert(key).second)
                    continue;
                if (!method.visible || method.protectedMember)
                    continue;
                if (selected)
                    refuse(origin, "ambiguous composition method selector '" + type.displayText() + "." + name + "'");
                selected = &method;
                selectedOwner = owner;
            }
            const auto found = types_.find(owner->baseClass);
            owner = found == types_.end() ? nullptr : found->second;
        }
        if (!selected || selected->identity.empty() || selected->asynchronous || !selected->typeParameters.empty())
            refuse(origin, "composition method selector requires one checked public synchronous nongeneric method");
        return {selectedOwner, selected};
    }

    void cleanupContracts(CompositionGraph& graph, const std::vector<const CompositionDeclaration*>& fragments) {
        std::map<std::string, CompositionCleanup> contracts;
        for (const auto* fragment : fragments) {
            for (const auto& selector : fragment->cleanupContracts) {
                const auto origin = originOf(*fragment, selector.offset);
                work(origin);
                const auto type = sourceType(selector.owner, origin);
                const auto* owner = declaration(type);
                if (!owner || !owner->sourceInterface)
                    refuse(origin, "cleanup selectors require an exact nominal source interface contract");
                const auto [methodOwner, method] = selectedMethod(type, selector.method, origin);
                if (method->parameters.size() != 1 || method->parameters.front().type.text() != "CancellationToken" ||
                    method->result.name != "Result" || method->result.arguments.size() != 2 ||
                    method->result.arguments[0].name != "void" || method->result.arguments[1].name != "string")
                    refuse(origin, "cleanup contract requires Result<void,string> Method(CancellationToken)");
                CompositionCleanup cleanup{type, method->name, method->identity, signature(*methodOwner, *method),
                    {methodOwner->module, methodOwner->originPath, method->offset}, origin};
                const auto key = type.name + ':' + method->name;
                contracts.emplace(key, std::move(cleanup));
                claimOrigin(origin);
            }
        }
        for (auto& [key, cleanup] : contracts)
            graph.cleanupContracts.push_back(std::move(cleanup));
    }

    CompositionGraph plan(const CompositionDeclaration& root, const std::string& identity) {
        CompositionGraph graph;
        graph.name = identity;
        graph.origin = originOf(root, root.offset);
        claimOrigin(graph.origin);
        bytes(identity.size(), graph.origin);
        std::map<std::string, Registration> registrations;
        const auto included = fragments(root);
        cleanupContracts(graph, included);
        for (const auto* fragment : included) {
            if (fragment != &root) {
                bytes(identityOf(*fragment).size(), graph.origin);
                graph.includes.push_back(identityOf(*fragment));
            }
            for (const auto& registration : fragment->registrations) {
                const auto origin = originOf(*fragment, registration.offset);
                work(origin);
                const auto service = sourceType(registration.service, origin);
                Type implementation;
                const Declaration* factoryOwner = nullptr;
                const Method* factoryMethod = nullptr;
                Type factoryReceiver;
                if (registration.factory) {
                    const auto selectorOrigin = originOf(*fragment, registration.factory->offset);
                    factoryReceiver = sourceType(registration.factory->owner, selectorOrigin);
                    const auto selected = selectedMethod(factoryReceiver, registration.factory->method, selectorOrigin);
                    factoryOwner = selected.first;
                    factoryMethod = selected.second;
                    const auto& result = factoryMethod->result;
                    if (result.name != "Result" || result.arguments.size() != 2 || result.arguments[1].name != "string")
                        refuse(selectorOrigin, "composition factories require Result<ConcreteSourceClass,string>");
                    implementation = sourceType(result.arguments[0], selectorOrigin);
                } else if (!registration.borrowed) {
                    implementation = sourceType(registration.implementation, origin);
                }
                const auto key = service.name;
                const auto previous = registrations.find(key);
                if (previous != registrations.end()) {
                    const auto& first = previous->second;
                    refuse(origin, "ambiguous composition binding for '" + service.displayText() + "'; first registered in " +
                        first.composition->originPath + " at offset " + std::to_string(first.syntax->offset));
                }
                registrations.emplace(key, Registration{fragment, &registration, service, implementation,
                    factoryOwner, factoryMethod, factoryReceiver});
            }
        }
        std::sort(graph.includes.begin(), graph.includes.end());
        std::vector<Registration> ordered;
        for (const auto& [key, registration] : registrations)
            ordered.push_back(registration);
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.service.displayText() < right.service.displayText();
        });
        std::map<std::string, std::size_t> nodes;
        for (const auto& registration : ordered) {
            const auto& syntax = *registration.syntax;
            CompositionService service;
            service.origin = originOf(*registration.composition, syntax.offset);
            claimOrigin(service.origin);
            bytes(registration.service.displayText().size(), service.origin);
            bytes(registration.implementation.displayText().size(), service.origin);
            service.service = registration.service;
            service.implementation = registration.implementation;
            service.lifetime = syntax.lifetime;
            if (syntax.borrowed) {
                if (syntax.lifetime == ServiceLifetime::Transient)
                    refuse(service.origin, "composition inputs require singleton or scoped lifetime guarantees");
                service.input = inputKind(service.service, service.origin);
            } else {
                const auto* implementation = declaration(service.implementation);
                if (!implementation || implementation->kind != "Class" || implementation->modelExported ||
                    !implementation->typeParameters.empty())
                    refuse(service.origin, "constructed composition implementations must be ordinary source classes, not host interfaces or native model owners");
                if (!compatible(*implementation, service.service))
                    refuse(service.origin, "composition implementation '" + service.implementation.displayText() +
                        "' is not assignable to service '" + service.service.displayText() + "'");
                if (registration.factoryMethod) {
                    const auto& selected = *registration.factoryMethod;
                    const auto* owner = registration.factoryOwner;
                    service.factory = CompositionFactory{registration.factoryReceiver, 0, selected.name,
                        selected.identity, signature(*owner, selected),
                        {owner->module, owner->originPath, selected.offset}, {}};
                } else {
                    const auto& selected = constructor(*implementation, service.origin);
                    service.constructorOrigin = {implementation->module, implementation->originPath, selected.offset};
                    claimOrigin(service.constructorOrigin);
                    bytes(selected.identity.size(), service.origin);
                    service.constructorSignature = signature(*implementation, selected);
                    bytes(service.constructorSignature.size(), service.origin);
                    service.constructorIdentity = selected.identity;
                }
                for (std::size_t cleanup = 0; cleanup < graph.cleanupContracts.size(); ++cleanup) {
                    if (!compatible(*implementation, graph.cleanupContracts[cleanup].contract))
                        continue;
                    if (service.cleanupIndex)
                        refuse(service.origin, "multiple cleanup contracts apply to constructed service '" + service.service.displayText() + "'");
                    service.cleanupIndex = cleanup;
                }
            }
            nodes.emplace(service.service.name, graph.services.size());
            graph.services.push_back(std::move(service));
        }
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            auto& service = graph.services[index];
            if (service.input != CompositionInput::None)
                continue;
            const auto* implementation = ordered[index].factoryOwner ? ordered[index].factoryOwner : declaration(service.implementation);
            const auto& selected = ordered[index].factoryMethod ? *ordered[index].factoryMethod : constructor(*implementation, service.origin);
            if (service.factory) {
                auto& factory = *service.factory;
                const auto receiver = nodes.find(factory.receiver.name);
                if (receiver == nodes.end())
                    refuse(service.origin, "factory receiver requires an explicit composition binding: '" + factory.receiver.displayText() + "'");
                factory.receiverIndex = receiver->second;
                service.dependencies.push_back({"$factoryReceiver", factory.receiver, receiver->second, service.origin});
            }
            bool hasToken = false;
            for (const auto& parameter : selected.parameters) {
                const CompositionOrigin origin{implementation->module, implementation->originPath, parameter.offset};
                work(origin);
                if (service.factory && parameter.type.text() == "CancellationToken") {
                    if (hasToken)
                        refuse(origin, "composition factories accept at most one CancellationToken parameter");
                    hasToken = true;
                    service.factory->parameters.push_back({parameter.name, parameter.type, std::nullopt});
                    continue;
                }
                const auto type = sourceType(parameter.type, origin);
                const auto dependency = nodes.find(type.name);
                if (dependency == nodes.end())
                    refuse(origin, std::string("missing composition binding for ") + (service.factory ? "factory" : "constructor") + " dependency '" + type.displayText() +
                        "' required by service '" + service.service.displayText() + "'");
                claimOrigin(origin);
                bytes(parameter.name.size(), origin);
                bytes(type.displayText().size(), origin);
                service.dependencies.push_back({parameter.name, type, dependency->second, origin});
                if (service.factory)
                    service.factory->parameters.push_back({parameter.name, type, dependency->second});
            }
        }
        order(graph);
        lifetimes(graph);
        std::set<std::string> exports;
        for (std::size_t index = 0; index < root.exports.size(); ++index) {
            const auto offset = index < root.exportOffsets.size() ? root.exportOffsets[index] : root.offset;
            const auto origin = originOf(root, offset);
            work(origin);
            const auto type = sourceType(root.exports[index], origin);
            const auto* exported = declaration(type);
            if (exported && exported->kind == "Interface" && !exported->sourceInterface)
                refuse(origin, "host interfaces are constructor-only composition inputs and cannot be exported");
            if (!exports.insert(type.name).second)
                refuse(origin, "duplicate composition export '" + type.displayText() + "'");
            const auto found = nodes.find(type.name);
            if (found == nodes.end())
                refuse(origin, "composition export has no explicit binding: '" + type.displayText() + "'");
            graph.exports.push_back(found->second);
        }
        std::sort(graph.exports.begin(), graph.exports.end());
        return graph;
    }

    void order(CompositionGraph& graph) {
        std::vector<std::size_t> pending(graph.services.size());
        std::vector<std::vector<std::size_t>> consumers(graph.services.size());
        std::set<std::size_t> ready;
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            pending[index] = graph.services[index].dependencies.size();
            for (const auto& dependency : graph.services[index].dependencies)
                consumers[dependency.target].push_back(index);
            if (pending[index] == 0)
                ready.insert(index);
        }
        while (!ready.empty()) {
            const auto index = *ready.begin();
            ready.erase(ready.begin());
            graph.constructionOrder.push_back(index);
            for (const auto consumer : consumers[index]) {
                work(graph.services[consumer].origin);
                if (--pending[consumer] == 0)
                    ready.insert(consumer);
            }
        }
        if (graph.constructionOrder.size() != graph.services.size())
            dependencyCycle(graph, pending);
    }

    void dependencyCycle(const CompositionGraph& graph, const std::vector<std::size_t>& pending) {
        struct Frame {
            std::size_t node;
            std::size_t next = 0;
        };
        std::vector<int> state(graph.services.size());
        std::vector<Frame> stack;
        for (std::size_t start = 0; start < graph.services.size(); ++start) {
            if (pending[start] == 0 || state[start] != 0)
                continue;
            state[start] = 1;
            stack.push_back({start, 0});
            while (!stack.empty()) {
                auto& frame = stack.back();
                const auto& service = graph.services[frame.node];
                if (frame.next == service.dependencies.size()) {
                    state[frame.node] = 2;
                    stack.pop_back();
                    continue;
                }
                const auto& edge = service.dependencies[frame.next++];
                work(edge.origin);
                if (pending[edge.target] == 0 || state[edge.target] == 2)
                    continue;
                if (state[edge.target] == 1) {
                    std::string path;
                    bool inCycle = false;
                    for (const auto& member : stack) {
                        inCycle = inCycle || member.node == edge.target;
                        if (!inCycle)
                            continue;
                        const auto name = graph.services[member.node].service.displayText();
                        bytes(name.size(), edge.origin);
                        if (!path.empty())
                            path += " -> ";
                        path += name;
                    }
                    path += " -> " + graph.services[edge.target].service.displayText();
                    refuse(edge.origin, "composition constructor dependency cycle: " + path);
                }
                state[edge.target] = 1;
                stack.push_back({edge.target, 0});
            }
        }
        throw std::logic_error("unresolved composition graph without a dependency cycle");
    }

    std::string scopedPath(const CompositionGraph& graph, std::size_t start) {
        std::vector<std::size_t> path{start};
        while (graph.services[path.back()].lifetime != ServiceLifetime::Scoped) {
            const auto& service = graph.services[path.back()];
            bool found = false;
            for (const auto& dependency : service.dependencies) {
                if (!graph.services[dependency.target].rootResolvable) {
                    work(service.origin);
                    path.push_back(dependency.target);
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
        std::string result;
        for (const auto index : path) {
            if (!result.empty())
                result += " -> ";
            result += graph.services[index].service.displayText();
            bytes(graph.services[index].service.displayText().size() + 4, graph.services[index].origin);
        }
        return result;
    }

    void lifetimes(CompositionGraph& graph) {
        for (const auto index : graph.constructionOrder) {
            auto& service = graph.services[index];
            service.scopeResolvable = true;
            service.rootResolvable = service.lifetime != ServiceLifetime::Scoped;
            for (const auto& dependency : service.dependencies)
                service.rootResolvable = service.rootResolvable && graph.services[dependency.target].rootResolvable;
            if (service.lifetime == ServiceLifetime::Singleton && !service.rootResolvable)
                refuse(service.origin, "singleton composition service captures a scoped dependency: " + scopedPath(graph, index));
        }
    }

    const Program& program_;
    std::map<std::string, const Declaration*> types_;
    std::map<std::string, const CompositionDeclaration*> compositions_;
    std::size_t work_ = 0;
    std::size_t metadataBytes_ = 0;
};

}

std::vector<CompositionGraph> planCompositions(const Program& program) {
    return Planner(program).run();
}

nlohmann::json compositionPlans(const Program& program) {
    auto result = nlohmann::json::array();
    std::size_t bytes = 2;
    for (const auto& graph : planCompositions(program)) {
        auto item = compositionGraphJson(graph);
        const auto encoded = item.dump().size();
        if (budgetExceeded(bytes, encoded, resourceBudget().outputBytes) ||
            budgetExceeded(bytes + encoded, 1, resourceBudget().outputBytes))
            refuse(graph.origin, "composition graph output_bytes resource budget exceeded; configure output_bytes");
        bytes += encoded + 1;
        result.push_back(std::move(item));
    }
    return result;
}

}
