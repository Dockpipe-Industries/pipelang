// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionEmitter.hpp"
#include "CompositionFactoryProfile.hpp"

#include <sstream>
#include <set>
#include <map>

namespace pipelang::models {
namespace {

class CompositionEmitter {
public:
    CompositionEmitter(const Program& program, const std::vector<CompositionGraph>& graphs,
            const CompositionFactoryBindings& bindings)
        : graphs_(graphs), bindings_(bindings), profile_(program, graphs) {}

    std::string run() {
        profile_.admit();
        line("// Generated checked composition with explicit resource ownership and scoped tasks.");
        line("#pragma once");
        line("#include \"Model.hpp\"");
        if (bindings_.compiled)
            line("#include \"Objects.hpp\"");
        line("#include <pipelang/di/ScopeExecution.hpp>");
        line("#include <pipelang/di/ConstructionAttempt.hpp>");
        line("#include <optional>");
        line("#include <algorithm>");
        line("#include <memory>");
        line("#include <string>");
        line("#include <utility>");
        line("#include <type_traits>");
        line("#include <vector>");
        line("namespace " + bindings_.nameSpace + " {");
        bridges();
        for (const auto& graph : graphs_)
            composition(graph);
        line("}");
        return output_.str();
    }

private:
    void line(const std::string& text = {}, unsigned indentation = 0) {
        output_ << std::string(indentation * 4, ' ') << text << '\n';
    }
    std::string cpp(const Type& type) const { return bindings_.cpp(type); }
    static std::string result(const std::string& value) {
        return "pipelang::model::Result<" + value + ", std::string>";
    }
    static std::string basename(const std::string& name) {
        const auto separator = name.rfind('.');
        return name.substr(separator == std::string::npos ? 0 : separator + 1);
    }
    static std::string moduleNamespace(const std::string& name) {
        std::string result;
        for (const auto character : name)
            result += character == '.' ? "::" : std::string(1, character);
        return result;
    }
    std::string exportName(const CompositionService& service) const {
        return basename(profile_.declaration(service.service)->sourceName);
    }
    std::string parameters(const Method& method, bool cancellation = false) const {
        std::string text;
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            const auto& parameter = method.parameters[index];
            if (!text.empty())
                text += ", ";
            text += cpp(parameter.type) + " pl_argument_" + std::to_string(index);
        }
        if (cancellation) {
            if (!text.empty())
                text += ", ";
            text += "pipelang::model::CancellationToken pl_cancellation = {}";
        }
        return text;
    }
    static std::string arguments(const Method& method) {
        std::string text;
        for (std::size_t index = 0; index < method.parameters.size(); ++index)
            text += ", std::move(pl_argument_" + std::to_string(index) + ")";
        return text;
    }
    void validateArgument(const Type& type, const std::string& name, unsigned indentation,
            bool domain = true) {
        const auto* owner = profile_.declaration(type);
        if (owner && owner->kind == "Interface" && !owner->sourceInterface) {
            line("if (!" + name + ")", indentation);
            line("throw pipelang::model::ExecutionFailure(\"composition host input is null\");", indentation + 1);
        } else if (owner && (owner->kind == "Class" || owner->sourceInterface)) {
            line(name + ".requireValid();", indentation);
            if (domain) {
                line("if (" + name + ".ownerDomain() != pl_execution.transaction.domain())", indentation);
                line("throw pipelang::model::ExecutionFailure(\"composition argument belongs to another object domain\");", indentation + 1);
            }
        } else {
            line("validate(" + name + ");", indentation);
        }
    }
    void bridges() {
        line("namespace construction {");
        line("// Only selected checked constructors, factory/cleanup methods and exported methods are admitted.");
        line("template<class Reference> struct SourceConstructor;");
        line("template<class Reference> struct SourceMethods;");
        std::set<std::string> constructors;
        std::map<std::string, std::pair<Type, std::vector<CompositionMethod>>> services;
        const auto addMethod = [&](const Type& type, const CompositionMethod& entry) {
            auto& selected = services[type.name];
            selected.first = type;
            if (std::none_of(selected.second.begin(), selected.second.end(), [&](const auto& previous) {
                    return previous.method->identity == entry.method->identity;
                }))
                selected.second.push_back(entry);
        };
        for (const auto& graph : graphs_) {
            for (const auto& service : graph.services) {
                if (service.factory)
                    addMethod(service.factory->receiver, profile_.checkedMethod(service.factory->identity));
                if (service.input != CompositionInput::None || service.factory ||
                    !constructors.insert(service.constructorIdentity).second)
                    continue;
                const auto& method = profile_.constructor(service.constructorIdentity);
                const auto reference = cpp(service.implementation);
                line("template<> struct SourceConstructor<" + reference + "> {");
                auto signature = "static " + reference + " Create(pipelang::model::ObjectExecution& pl_execution";
                const auto params = parameters(method);
                if (!params.empty())
                    signature += ", " + params;
                line(signature + ") {", 1);
                line("pl_execution.scope.requireOwner();", 2);
                line("using pipelang::model::validate;", 2);
                for (std::size_t index = 0; index < method.parameters.size(); ++index)
                    validateArgument(method.parameters[index].type, "pl_argument_" + std::to_string(index), 2);
                line("return " + bindings_.callable(method.identity) + "(pl_execution" + arguments(method) + ");", 2);
                line("}", 1);
                line("};");
            }
            for (const auto index : graph.exports) {
                const auto& service = graph.services[index];
                for (const auto& entry : profile_.methods(service.service))
                    addMethod(service.service, entry);
            }
            for (const auto& cleanup : graph.cleanupContracts)
                addMethod(cleanup.contract, profile_.checkedMethod(cleanup.identity));
        }
        for (const auto& [key, selected] : services) {
                const auto& type = selected.first;
                const auto reference = cpp(type);
                line("template<> struct SourceMethods<" + reference + "> {");
                for (const auto& entry : selected.second) {
                    const auto& method = *entry.method;
                    const auto returned = method.asynchronous ? "pipelang::model::Coroutine<" + cpp(method.result.arguments.at(0)) + ">" : cpp(method.result);
                    auto signature = "static " + returned + " " + method.name +
                        "(pipelang::model::ObjectExecution& pl_execution, " + reference + " pl_receiver";
                    const auto params = parameters(method);
                    if (!params.empty())
                        signature += ", " + params;
                    if (method.asynchronous)
                        signature += ", pipelang::model::CancellationToken pl_taskCancellation";
                    line(signature + ") {", 1);
                    line("pl_execution.scope.requireOwner();", 2);
                    line("using pipelang::model::validate;", 2);
                    validateArgument(type, "pl_receiver", 2);
                    for (std::size_t index = 0; index < method.parameters.size(); ++index)
                        validateArgument(method.parameters[index].type, "pl_argument_" + std::to_string(index), 2);
                    // Authored tokens remain independent of the native task
                    // boundary. Concrete operations choose which token to use.
                    const Type owner{entry.owner->name, {}, entry.owner->sourceName};
                    auto function = bindings_.callable(method.identity);
                    if (entry.owner->kind == "Class" && method.virtualMethod)
                        function += "_dispatch";
                    line("return " + function + "(pl_execution, pl_receiver.template as<" +
                        bindings_.tag(owner) + ">()" + arguments(method) +
                        (method.asynchronous ? ", pl_taskCancellation" : "") + ");", 2);
                    line("}", 1);
                }
                line("};");
        }
        line("template<class Reference, class... Arguments>");
        line("Reference Construct(pipelang::model::ObjectExecution& execution, Arguments&&... arguments) {");
        line("return SourceConstructor<Reference>::Create(execution, std::forward<Arguments>(arguments)...);", 1);
        line("}");
        line("}");
    }
    std::string inputParameters(const CompositionGraph& graph, ServiceLifetime lifetime) const {
        std::string text;
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input == CompositionInput::None || service.lifetime != lifetime)
                continue;
            if (!text.empty())
                text += ", ";
            text += cpp(service.service) + " pl_input_" + std::to_string(index);
        }
        return text;
    }
    std::string inputArguments(const CompositionGraph& graph, ServiceLifetime lifetime) const {
        std::string text;
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input == CompositionInput::None || service.lifetime != lifetime)
                continue;
            if (!text.empty())
                text += ", ";
            text += "std::move(pl_input_" + std::to_string(index) + ")";
        }
        return text;
    }
    void inputStruct(const CompositionGraph& graph, const std::string& name, ServiceLifetime lifetime) {
        line("struct " + name + " {", 1);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input != CompositionInput::None && service.lifetime == lifetime)
                line(cpp(service.service) + " pl_input_" + std::to_string(index) + ";", 2);
        }
        line("};", 1);
    }
    void cacheStruct(const CompositionGraph& graph, const std::string& name, ServiceLifetime lifetime) {
        line("struct " + name + " {", 1);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input == CompositionInput::None && service.lifetime == lifetime)
                line("std::optional<" + cpp(service.service) + "> pl_cache_" + std::to_string(index) + ";", 2);
        }
        line("};", 1);
    }
    void composition(const CompositionGraph& graph) {
        const auto name = basename(graph.name);
        const auto module = graph.name.substr(0, graph.name.rfind('.'));
        line("namespace composition::" + moduleNamespace(module) + " {");
        line("class " + name + " {");
        line("public:");
        line("class Root;", 1);
        line("class Scope;", 1);
        for (const auto index : graph.exports)
            line("class " + exportName(graph.services[index]) + "Handle;", 1);
        line("static " + result("std::shared_ptr<Root>") + " Create(" + inputParameters(graph, ServiceLifetime::Singleton) + ");", 1);
        line("private:");
        line("friend class Root;", 1);
        line("friend class Scope;", 1);
        inputStruct(graph, "RootInputs", ServiceLifetime::Singleton);
        inputStruct(graph, "ScopeInputs", ServiceLifetime::Scoped);
        cacheStruct(graph, "RootCache", ServiceLifetime::Singleton);
        cacheStruct(graph, "ScopeCache", ServiceLifetime::Scoped);
        line("struct Retained {", 1);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input == CompositionInput::None && service.lifetime == ServiceLifetime::Transient)
                line("std::vector<" + cpp(service.service) + "> pl_values_" + std::to_string(index) + ";", 2);
        }
        line("};", 1);
        line("struct Stage {", 1);
        line("RootCache root;", 2);
        line("ScopeCache scope;", 2);
        line("Retained rootPending;", 2);
        line("Retained scopePending;", 2);
        line("};", 1);
        line("template<class T, class U>", 1);
        line("static " + result("T") + " pl_failure(const " + result("U") + "& value) {", 1);
        line("if (value.is_cancelled())", 2);
        line("return " + result("T") + "::Cancelled(value.error());", 3);
        line("return " + result("T") + "::Failure(value.error());", 2);
        line("}", 1);
        retentionHelpers();
        line("static void pl_publish(Root&, Scope*, Stage&);", 1);
        for (std::size_t index = 0; index < graph.services.size(); ++index)
            line("static " + cpp(graph.services[index].service) + " pl_resolve_" + std::to_string(index) +
                "(Root&, Scope*, Stage&, pipelang::model::ObjectExecution&, pipelang::di::ConstructionAttempt&, pipelang::model::CancellationToken, bool);", 1);
        line("};");
        rootClass(graph, name);
        scopeClass(graph, name);
        for (const auto index : graph.exports)
            handleClass(graph.services[index], name);
        bootstrap(graph, name);
        recipes(graph, name);
        publication(graph, name);
        for (const auto index : graph.exports) {
            const auto& service = graph.services[index];
            if (service.rootResolvable)
                resolver(graph, name, index, false);
            resolver(graph, name, index, true);
            handleMethods(service, name);
        }
        line("}");
    }
    void retentionHelpers() {
        line("template<class T>", 1);
        line("static void pl_reserve(std::vector<T>& values, std::size_t needed) {", 1);
        line("constexpr auto maximum = pipelang::model::ObjectDomain::maximumCells;", 2);
        line("if (needed > maximum)", 2);
        line("throw pipelang::model::ExecutionFailure(\"composition retention capacity exceeded\");", 3);
        line("if (needed <= values.capacity())", 2);
        line("return;", 3);
        line("const auto grown = values.capacity() > maximum / 2 ? maximum : std::max<std::size_t>(8, values.capacity() * 2);", 2);
        line("values.reserve(std::max(needed, grown));", 2);
        line("}", 1);
        line("template<class T>", 1);
        line("static void pl_prepare(std::vector<T>& retained, std::vector<T>& pending) {", 1);
        line("constexpr auto maximum = pipelang::model::ObjectDomain::maximumCells;", 2);
        line("if (pending.size() >= maximum || retained.size() > maximum - pending.size() - 1)", 2);
        line("throw pipelang::model::ExecutionFailure(\"composition retention capacity exceeded\");", 3);
        line("pl_reserve(retained, retained.size() + pending.size() + 1);", 2);
        line("pl_reserve(pending, pending.size() + 1);", 2);
        line("}", 1);
        line("template<class T>", 1);
        line("static void pl_append(std::vector<T>& retained, std::vector<T>& pending) noexcept {", 1);
        line("static_assert(std::is_nothrow_move_constructible_v<T>);", 2);
        line("if (retained.capacity() - retained.size() < pending.size())", 2);
        line("std::terminate();", 3);
        line("for (auto& value : pending)", 2);
        line("retained.push_back(std::move(value));", 3);
        line("}", 1);
    }
    void rootClass(const CompositionGraph& graph, const std::string& name) {
        line("class " + name + "::Root : public std::enable_shared_from_this<Root> {");
        line("public:");
        line("Root(const Root&) = delete;", 1);
        line("Root& operator=(const Root&) = delete;", 1);
        line("Root(Root&&) = delete;", 1);
        line("Root& operator=(Root&&) = delete;", 1);
        line(result("std::shared_ptr<Scope>") + " CreateScope(" + inputParameters(graph, ServiceLifetime::Scoped) + ");", 1);
        lifecycleMethods(true);
        for (const auto index : graph.exports) {
            const auto& service = graph.services[index];
            if (service.rootResolvable)
                line(result(exportName(service) + "Handle") + " Resolve" + exportName(service) +
                    "(pipelang::model::CancellationToken pl_cancellation = {});", 1);
        }
        line("private:");
        line("friend class " + name + ";", 1);
        line("friend class Scope;", 1);
        for (const auto index : graph.exports)
            line("friend class " + exportName(graph.services[index]) + "Handle;", 1);
        line("Root(std::shared_ptr<pipelang::di::ExecutionOwner> owner, std::shared_ptr<pipelang::di::ScopeExecution> execution, RootInputs inputs)", 1);
        line(": pl_owner_(std::move(owner)), pl_execution_(std::move(execution)), pl_inputs_(std::move(inputs)) {}", 2);
        line("std::shared_ptr<pipelang::di::ExecutionOwner> pl_owner_;", 1);
        line("std::shared_ptr<pipelang::di::ScopeExecution> pl_execution_;", 1);
        line("RootInputs pl_inputs_;", 1);
        line("RootCache pl_cache_;", 1);
        line("Retained pl_retained_;", 1);
        line("};");
    }
    void scopeClass(const CompositionGraph& graph, const std::string& name) {
        line("class " + name + "::Scope : public std::enable_shared_from_this<Scope> {");
        line("public:");
        line("Scope(const Scope&) = delete;", 1);
        line("Scope& operator=(const Scope&) = delete;", 1);
        line("Scope(Scope&&) = delete;", 1);
        line("Scope& operator=(Scope&&) = delete;", 1);
        lifecycleMethods(false);
        for (const auto index : graph.exports) {
            const auto& service = graph.services[index];
            line(result(exportName(service) + "Handle") + " Resolve" + exportName(service) +
                "(pipelang::model::CancellationToken pl_cancellation = {});", 1);
        }
        line("private:");
        line("friend class " + name + ";", 1);
        line("friend class Root;", 1);
        for (const auto index : graph.exports)
            line("friend class " + exportName(graph.services[index]) + "Handle;", 1);
        line("Scope(std::shared_ptr<Root> root, ScopeInputs inputs)", 1);
        line(": pl_root_(std::move(root)), pl_inputs_(std::move(inputs)) {}", 2);
        line("std::shared_ptr<Root> pl_root_;", 1);
        line("std::shared_ptr<pipelang::di::ScopeExecution> pl_execution_;", 1);
        line("ScopeInputs pl_inputs_;", 1);
        line("ScopeCache pl_cache_;", 1);
        line("Retained pl_retained_;", 1);
        line("};");
    }
    void lifecycleMethods(bool root) {
        line(result("pipelang::di::CleanupSession") + " ObserveCleanup() { return pl_execution_->cleanupSession(); }", 1);
        line(result("pipelang::di::CleanupStatus") + " CleanupStatus() {", 1);
        line("auto pl_status = pl_execution_->cleanupStatus();", 2);
        line("if (pl_status.is_success() && pl_status.value().phase == pipelang::di::ScopePhase::Closed) {", 2);
        line(std::string("pl_cache_ = ") + (root ? "RootCache{};" : "ScopeCache{};"), 3);
        line("pl_retained_ = Retained{};", 3);
        line("}", 2);
        line("return pl_status;", 2);
        line("}", 1);
        line(result("void") + " Close() {", 1);
        line("auto pl_outcome = pl_execution_->close();", 2);
        line("(void)CleanupStatus();", 2);
        line("return pl_outcome;", 2);
        line("}", 1);
        line(result("pipelang::model::Task<" + result("void") + ">") + " CloseAsync() {", 1);
        line("return pl_execution_->closeAsync();", 2);
        line("}", 1);
        line(result("std::size_t") + " Poll() {", 1);
        line("auto pl_outcome = pl_execution_->poll();", 2);
        line("(void)CleanupStatus();", 2);
        line("return pl_outcome;", 2);
        line("}", 1);
        line(result("std::size_t") + " Pending() { return pl_execution_->pending(); }", 1);
    }
    void handleClass(const CompositionService& service, const std::string& name) {
        const auto handle = exportName(service) + "Handle";
        line("class " + name + "::" + handle + " {");
        line("public:");
        for (const auto& entry : profile_.methods(service.service)) {
            const auto& method = *entry.method;
            const auto returned = facadeResult(method);
            line(returned + " " + method.name + "(" + parameters(method, true) + ") const;", 1);
        }
        line("private:");
        line("friend class Root;", 1);
        line("friend class Scope;", 1);
        line(handle + "(std::shared_ptr<Root> root, std::shared_ptr<Scope> scope, " + cpp(service.service) + " reference)", 1);
        line(": pl_root_(std::move(root)), pl_scope_(std::move(scope)), pl_reference_(std::move(reference)) {}", 2);
        line("std::shared_ptr<Root> pl_root_;", 1);
        line("std::shared_ptr<Scope> pl_scope_;", 1);
        line(cpp(service.service) + " pl_reference_;", 1);
        line("};");
    }
    std::string facadeResult(const Method& method) const {
        if (method.asynchronous)
            return result(cpp(method.result));
        if (method.result.name == "Result" && method.result.arguments.at(1).name == "string")
            return cpp(method.result);
        return result(cpp(method.result));
    }
    void bootstrap(const CompositionGraph& graph, const std::string& name) {
        const auto rootResult = result("std::shared_ptr<" + name + "::Root>");
        line("inline " + rootResult + " " + name + "::Create(" + inputParameters(graph, ServiceLifetime::Singleton) + ") {");
        line("try {", 1);
        line("using pipelang::model::validate;", 2);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input != CompositionInput::None && service.lifetime == ServiceLifetime::Singleton)
                validateArgument(service.service, "pl_input_" + std::to_string(index), 2, false);
        }
        line("auto owner = pipelang::di::ExecutionOwner::Create();", 2);
        line("if (!owner.is_success()) return pl_failure<std::shared_ptr<Root>>(owner);", 2);
        line("auto execution = owner.value()->rootExecution();", 2);
        line("if (!execution.is_success()) return pl_failure<std::shared_ptr<Root>>(execution);", 2);
        line("return " + rootResult + "::Success(std::shared_ptr<Root>(new Root(owner.value(), execution.value(), RootInputs{" +
            inputArguments(graph, ServiceLifetime::Singleton) + "})));", 2);
        catches(rootResult);
        line("}");
        const auto scopeResult = result("std::shared_ptr<" + name + "::Scope>");
        line("inline " + scopeResult + " " + name + "::Root::CreateScope(" + inputParameters(graph, ServiceLifetime::Scoped) + ") {");
        line("const auto available = pl_execution_->checkOpen();", 1);
        line("if (!available.is_success()) return pl_failure<std::shared_ptr<Scope>>(available);", 1);
        line("try {", 1);
        line("using pipelang::model::validate;", 2);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input != CompositionInput::None && service.lifetime == ServiceLifetime::Scoped)
                validateArgument(service.service, "pl_input_" + std::to_string(index), 2, false);
        }
        line("auto scope = std::shared_ptr<Scope>(new Scope(shared_from_this(), ScopeInputs{" + inputArguments(graph, ServiceLifetime::Scoped) + "}));", 2);
        line("auto execution = pl_owner_->createScope(pl_execution_);", 2);
        line("if (!execution.is_success()) return pl_failure<std::shared_ptr<Scope>>(execution);", 2);
        line("scope->pl_execution_ = execution.value();", 2);
        line("return " + scopeResult + "::Success(std::move(scope));", 2);
        catches(scopeResult);
        line("}");
    }
    void catches(const std::string& returned) {
        line("} catch (const pipelang::model::OperationCancelled& error) {", 1);
        line("return " + returned + "::Cancelled(pipelang::model::safeErrorMessage(error));", 2);
        line("} catch (const std::exception& error) {", 1);
        line("return " + returned + "::Failure(pipelang::model::safeErrorMessage(error));", 2);
        line("}", 1);
    }
    void recipes(const CompositionGraph& graph, const std::string& name) {
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            const auto suffix = std::to_string(index);
            line("inline " + cpp(service.service) + " " + name + "::pl_resolve_" + suffix +
                "(Root& root, Scope* scope, Stage& stage, pipelang::model::ObjectExecution& pl_execution, pipelang::di::ConstructionAttempt& pl_attempt, pipelang::model::CancellationToken pl_cancellation, bool rootContext) {");
            line("pl_execution.budget.step();", 1);
            line("(void)root; (void)scope; (void)stage; (void)rootContext; (void)pl_attempt; (void)pl_cancellation;", 1);
            if (service.input != CompositionInput::None) {
                if (service.lifetime == ServiceLifetime::Singleton) {
                    line("return root.pl_inputs_.pl_input_" + suffix + ";", 1);
                } else {
                    line("if (rootContext || !scope)", 1);
                    line("throw pipelang::model::ExecutionFailure(\"scoped composition input requested from root\");", 2);
                    line("return scope->pl_inputs_.pl_input_" + suffix + ";", 1);
                }
                line("}");
                continue;
            }
            if (service.lifetime == ServiceLifetime::Singleton) {
                line("auto& cached = stage.root.pl_cache_" + suffix + ";", 1);
                line("rootContext = true;", 1);
            } else if (service.lifetime == ServiceLifetime::Scoped) {
                line("if (rootContext || !scope)", 1);
                line("throw pipelang::model::ExecutionFailure(\"scoped composition service requested from root\");", 2);
                line("auto& cached = stage.scope.pl_cache_" + suffix + ";", 1);
            }
            if (service.lifetime != ServiceLifetime::Transient) {
                line("if (cached)", 1);
                line("return *cached;", 2);
            }
            for (std::size_t dependency = 0; dependency < service.dependencies.size(); ++dependency)
                line("auto argument" + std::to_string(dependency) + " = pl_resolve_" +
                    std::to_string(service.dependencies[dependency].target) + "(root, scope, stage, pl_execution, pl_attempt, pl_cancellation, rootContext);", 1);
            if (service.lifetime == ServiceLifetime::Transient) {
                line("if (!rootContext && !scope)", 1);
                line("throw pipelang::model::ExecutionFailure(\"composition scope is unavailable\");", 2);
                line("auto& retained = rootContext ? root.pl_retained_.pl_values_" + suffix + " : scope->pl_retained_.pl_values_" + suffix + ";", 1);
                line("auto& pending = rootContext ? stage.rootPending.pl_values_" + suffix + " : stage.scopePending.pl_values_" + suffix + ";", 1);
                line("pl_prepare(retained, pending);", 1);
            }
            if (service.cleanupIndex) {
                const auto& cleanup = graph.cleanupContracts.at(*service.cleanupIndex);
                line("auto pl_reservation = pl_attempt.reserve<" + cpp(service.implementation) + ">(", 1);
                line("rootContext ? root.pl_execution_ : scope->pl_execution_, \"" + graph.name + ":" + service.service.displayText() + "\",", 2);
                line("[](" + cpp(service.implementation) + " pl_resource, pipelang::model::ObjectExecution& pl_cleanupExecution, pipelang::model::CancellationToken pl_cleanupToken) {", 2);
                line("return ::" + bindings_.nameSpace + "::construction::SourceMethods<" + cpp(cleanup.contract) + ">::" + cleanup.method +
                    "(pl_cleanupExecution, pl_resource.template as<" + bindings_.tag(cleanup.contract) + ">(), pl_cleanupToken);", 3);
                line("});", 2);
            }
            if (service.factory || service.cleanupIndex)
                line("const auto pl_allocationMark = pl_attempt.allocationMark();", 1);
            if (service.factory) {
                const auto& factory = *service.factory;
                std::string call = "::" + bindings_.nameSpace + "::construction::SourceMethods<" + cpp(factory.receiver) + ">::" + factory.method + "(pl_execution, std::move(argument0)";
                std::size_t dependency = 1;
                for (const auto& parameter : factory.parameters) {
                    if (parameter.target)
                        call += ", std::move(argument" + std::to_string(dependency++) + ")";
                    else
                        call += ", pl_cancellation";
                }
                line("auto pl_created = " + call + ");", 1);
                line("if (pl_created.is_cancelled())", 1);
                line("throw pipelang::model::OperationCancelled(pl_created.error());", 2);
                line("if (!pl_created.is_success())", 1);
                line("throw pipelang::model::ExecutionFailure(pl_created.error());", 2);
                line("auto pl_concrete = pl_created.value();", 1);
            } else {
                auto call = "::" + bindings_.nameSpace + "::construction::Construct<" + cpp(service.implementation) + ">(pl_execution";
                for (std::size_t dependency = 0; dependency < service.dependencies.size(); ++dependency)
                    call += ", std::move(argument" + std::to_string(dependency) + ")";
                line("auto pl_concrete = " + call + ");", 1);
            }
            if (service.cleanupIndex)
                line("pl_attempt.adopt(pl_reservation, pl_concrete, pl_allocationMark);", 1);
            else if (service.factory)
                line("pl_attempt.requireFresh(pl_concrete, pl_allocationMark);", 1);
            line("auto value = pl_concrete.template as<" + bindings_.tag(service.service) + ">();", 1);
            if (service.lifetime == ServiceLifetime::Transient)
                line("pending.push_back(value);", 1);
            else
                line("cached = value;", 1);
            line("return value;", 1);
            line("}");
        }
    }
    void publication(const CompositionGraph& graph, const std::string& name) {
        line("inline void " + name + "::pl_publish(Root& root, Scope* scope, Stage& stage) {");
        line("static_assert(std::is_nothrow_swappable_v<RootCache> && std::is_nothrow_swappable_v<ScopeCache>);", 1);
        line("using std::swap;", 1);
        line("swap(root.pl_cache_, stage.root);", 1);
        line("if (scope) swap(scope->pl_cache_, stage.scope);", 1);
        for (std::size_t index = 0; index < graph.services.size(); ++index) {
            const auto& service = graph.services[index];
            if (service.input == CompositionInput::None && service.lifetime == ServiceLifetime::Transient) {
                const auto field = "pl_values_" + std::to_string(index);
                line("pl_append(root.pl_retained_." + field + ", stage.rootPending." + field + ");", 1);
                line("if (scope) pl_append(scope->pl_retained_." + field + ", stage.scopePending." + field + ");", 1);
            }
        }
        line("}");
    }
    void resolver(const CompositionGraph& graph, const std::string& name, std::size_t index, bool scoped) {
        const auto& service = graph.services[index];
        const auto handle = exportName(service) + "Handle";
        const auto returned = result(name + "::" + handle);
        const auto context = scoped ? "Scope" : "Root";
        line("inline " + returned + " " + name + "::" + context + "::Resolve" + exportName(service) +
            "(pipelang::model::CancellationToken pl_cancellation) {");
        line("const auto available = pl_execution_->checkOpen(pl_cancellation);", 1);
        line("if (!available.is_success()) return pl_failure<" + handle + ">(available);", 1);
        line(scoped ? "auto root = pl_root_;" : "auto root = shared_from_this();", 1);
        const auto scope = scoped ? "this" : "nullptr";
        const auto handleScope = scoped && service.lifetime != ServiceLifetime::Singleton ? "shared_from_this()" : "std::shared_ptr<Scope>{}";
        if (service.input == CompositionInput::None && service.lifetime != ServiceLifetime::Transient) {
            const auto cache = service.lifetime == ServiceLifetime::Singleton ? "root->pl_cache_." : "pl_cache_.";
            const auto field = cache + std::string("pl_cache_") + std::to_string(index);
            line("if (" + field + ")", 1);
            line("return " + returned + "::Success(" + handle + "(root, " + handleScope + ", *" + field + "));", 2);
        }
        line("std::optional<Stage> stage;", 1);
        line("auto outcome = pl_execution_->construct<" + cpp(service.service) + ">(pl_cancellation, [&](pipelang::model::ObjectExecution& execution, pipelang::di::ConstructionAttempt& pl_attempt) {", 1);
        line("stage.emplace(Stage{root->pl_cache_, " + std::string(scoped ? "pl_cache_" : "ScopeCache{}") + ", {}, {}});", 2);
        line("auto value = pl_resolve_" + std::to_string(index) + "(*root, " + scope + ", *stage, execution, pl_attempt, pl_cancellation, " + (scoped ? "false" : "true") + ");", 2);
        line("if (pl_cancellation.is_cancelled())", 2);
        line("throw pipelang::model::OperationCancelled(\"composition construction cancelled\");", 3);
        line("return value;", 2);
        line("});", 1);
        line("if (!outcome.is_success()) return pl_failure<" + handle + ">(outcome);", 1);
        line("pl_publish(*root, " + std::string(scope) + ", *stage);", 1);
        line("return " + returned + "::Success(" + handle + "(root, " + handleScope + ", outcome.value()));", 1);
        line("}");
    }
    void handleMethods(const CompositionService& service, const std::string& name) {
        const auto handle = exportName(service) + "Handle";
        for (const auto& entry : profile_.methods(service.service)) {
            const auto& method = *entry.method;
            const auto returned = facadeResult(method);
            auto params = parameters(method, true);
            const auto defaultAt = params.rfind(" = {}");
            if (defaultAt != std::string::npos)
                params.erase(defaultAt);
            line("inline " + returned + " " + name + "::" + handle + "::" + method.name + "(" + params + ") const {");
            line("if (!pl_root_)", 1);
            line("return " + returned + "::Failure(\"composition handle is empty\");", 2);
            line("auto pl_context = pl_scope_ ? pl_scope_->pl_execution_ : pl_root_->pl_execution_;", 1);
            const auto call = "::" + bindings_.nameSpace + "::construction::SourceMethods<" + cpp(service.service) + ">::" + method.name +
                "(pl_execution, pl_reference_" + arguments(method) + ")";
            if (method.asynchronous) {
                line("try {", 1);
                // Only the separate boundary token controls task admission.
                std::string captures = "pl_receiver = pl_reference_";
                for (std::size_t index = 0; index < method.parameters.size(); ++index) {
                    const auto argument = "pl_argument_" + std::to_string(index);
                    captures += ", " + argument + " = std::move(" + argument + ")";
                }
                line("return pl_context->start<" + cpp(method.result.arguments.at(0).arguments.at(0)) + ">(pl_cancellation,", 2);
                line("[" + captures + "](pipelang::model::ObjectExecution& pl_execution, pipelang::model::CancellationToken pl_taskCancellation) mutable {", 3);
                line("return ::" + bindings_.nameSpace + "::construction::SourceMethods<" + cpp(service.service) + ">::" + method.name +
                    "(pl_execution, pl_receiver" + arguments(method) + ", pl_taskCancellation);", 4);
                line("});", 3);
                catches(returned);
            } else if (method.result.name != "Result" || method.result.arguments.at(1).name != "string")
                line("return pl_context->invoke<" + cpp(method.result) + ">(pl_cancellation, [&](pipelang::model::ObjectExecution& pl_execution) { return " + call + "; });", 1);
            else {
                line("auto pl_outcome = pl_context->invoke<" + cpp(method.result) + ">(pl_cancellation, [&](pipelang::model::ObjectExecution& pl_execution) { return " + call + "; });", 1);
                line("if (!pl_outcome.is_success()) return pl_failure<" + cpp(method.result.arguments[0]) + ">(pl_outcome);", 1);
                line("return pl_outcome.value();", 1);
            }
            line("}");
        }
    }

    const std::vector<CompositionGraph>& graphs_;
    const CompositionFactoryBindings& bindings_;
    CompositionFactoryProfile profile_;
    std::ostringstream output_;
};

}

std::string emitCompositionFactory(const Program& program, const std::vector<CompositionGraph>& graphs,
        const CompositionFactoryBindings& bindings) {
    return CompositionEmitter(program, graphs, bindings).run();
}

}
