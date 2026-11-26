// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/CompositionPlanner.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"
#include "../../src/native/model-compiler/composition.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pipelang::models;
using Json = nlohmann::json;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

Program checked(std::vector<ModuleSource> modules, const std::string& entry = "Services") {
    auto program = linkModules(modules, entry, true, true);
    program.objectEnabled = true;
    for (const auto& declaration : program.declarations) {
        if (declaration.modelDeclared)
            program.modelExports.push_back(declaration.sourceName);
    }
    normalizeComposition(program);
    check(program);
    return program;
}

Program checked(const std::string& source) {
    return checked({ModuleSource{"Services", "module Services;\n" + source, "case.pipe"}});
}

const CompositionGraph& graphNamed(const std::vector<CompositionGraph>& graphs, const std::string& name) {
    const auto found = std::find_if(graphs.begin(), graphs.end(), [&](const auto& graph) { return graph.name == name; });
    if (found == graphs.end())
        throw std::runtime_error("missing graph " + name);
    return *found;
}

const CompositionService& serviceNamed(const CompositionGraph& graph, const std::string& name) {
    const auto found = std::find_if(graph.services.begin(), graph.services.end(), [&](const auto& service) {
        return service.service.displayText() == name;
    });
    if (found == graph.services.end())
        throw std::runtime_error("missing service " + name);
    return *found;
}

void refused(const std::string& source, const std::string& category,
             const std::string& token = "", const std::string& path = "case.pipe") {
    const std::string complete = "module Services;\n" + source;
    try {
        const auto program = checked(source);
        const auto graphs = planCompositions(program);
        static_cast<void>(graphs);
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(category) != std::string::npos,
                "wrong diagnostic for " + category + ": " + diagnostic.what());
        require(diagnostic.path == path, "diagnostic lost source path for " + category);
        if (!token.empty())
            require(diagnostic.offset == complete.find(token), "diagnostic lost exact token offset for " + category);
        return;
    }
    throw std::runtime_error("invalid composition accepted: " + category);
}

void typedInputsAndConstructors() {
    const auto program = checked(R"pipe(
public Interface IClock { pure int Now(); }
public source Interface IStore { pure int Read(); }
public Class Store : IStore {
    private IClock clock;
    public Store(IClock value) { clock = value; }
    public pure int Read() => clock.Now();
}
public Class UseCase {
    private IStore store;
    public UseCase(IStore value) { store = value; }
}
public composition Startup {
    singleton input IClock;
    singleton IStore = Store;
    transient UseCase = UseCase;
    export UseCase;
}
)pipe");
    const auto graphs = planCompositions(program);
    const auto& graph = graphNamed(graphs, "Services.Startup");
    require(graph.services.size() == 3, "typed graph lost services");
    const auto& clock = serviceNamed(graph, "Services.IClock");
    const auto& store = serviceNamed(graph, "Services.IStore");
    const auto& useCase = serviceNamed(graph, "Services.UseCase");
    require(clock.input == CompositionInput::Borrowed && clock.dependencies.empty(), "host input was constructed");
    require(store.constructorIdentity != "" && store.constructorSignature == "Services.Store(Services.IClock)", "checked constructor identity/signature lost");
    require(store.constructorOrigin.path == "case.pipe", "constructor origin lost");
    require(store.dependencies.size() == 1 && store.dependencies[0].type.displayText() == "Services.IClock", "constructor parameter type lost");
    require(store.rootResolvable && useCase.rootResolvable && useCase.scopeResolvable, "root transient/singleton context incorrect");
    require(graph.exports.size() == 1 && graph.services[graph.exports[0]].service.displayText() == "Services.UseCase", "explicit export incorrect");
    const auto json = compositionPlans(program);
    require(json[0]["checkpoint"] == "graph-only", "plan claimed runtime construction");
    require(json[0]["construction_order"][0] == "Services.IClock", "input not ordered before consumers");
    const auto index = store.dependencies[0].target;
    require(graph.services[index].service.displayText() == "Services.IClock", "dependency target index incorrect");
}

void lifetimeContexts() {
    const auto program = checked(R"pipe(
public Record TenantContext { public string tenantId = ""; }
public Class RequestUseCase { public RequestUseCase(TenantContext context) {} }
public composition Startup {
    scoped input TenantContext;
    transient RequestUseCase = RequestUseCase;
    export RequestUseCase;
}
)pipe");
    const auto graphs = planCompositions(program);
    const auto& graph = graphNamed(graphs, "Services.Startup");
    const auto& input = serviceNamed(graph, "Services.TenantContext");
    const auto& consumer = serviceNamed(graph, "Services.RequestUseCase");
    require(input.input == CompositionInput::Snapshot, "record input not classified as snapshot");
    require(!input.rootResolvable && input.scopeResolvable, "scoped snapshot allowed at root");
    require(!consumer.rootResolvable && consumer.scopeResolvable, "transient scoped dependency context lost");

    refused(R"pipe(
public Class Request {}
public Class Middle { public Middle(Request request) {} }
public Class Root { public Root(Middle middle) {} }
public composition Startup {
    scoped Request = Request;
    transient Middle = Middle;
    singleton Root = Root;
}
)pipe", "Services.Root -> Services.Middle -> Services.Request", "singleton Root");
    refused(R"pipe(
public Record TenantContext { public string tenantId = ""; }
public Class Root { public Root(TenantContext context) {} }
public composition Startup {
    scoped input TenantContext;
    singleton Root = Root;
}
)pipe", "singleton composition service captures", "singleton Root");
    refused(R"pipe(
public Class Root { public Root(int configuration) {} }
public composition Startup {
    scoped input int;
    singleton Root = Root;
}
)pipe", "singleton composition service captures", "singleton Root");
}

void valuesAndNominalCompatibility() {
    const auto program = checked(R"pipe(
public Enum Mode { First, Second }
public Record Settings { public int count = 0; }
public Class Consumer { public Consumer(Mode mode, Settings settings, string name, ByteBuffer bytes) {} }
public composition Startup {
    singleton input Mode;
    singleton input Settings;
    singleton input string;
    singleton input ByteBuffer;
    transient Consumer = Consumer;
}
)pipe");
    const auto graphs = planCompositions(program);
    const auto& graph = graphNamed(graphs, "Services.Startup");
    require(serviceNamed(graph, "Services.Mode").input == CompositionInput::Snapshot, "enum input not a snapshot");
    require(serviceNamed(graph, "string").input == CompositionInput::Snapshot, "primitive input not a snapshot");
    require(serviceNamed(graph, "ByteBuffer").input == CompositionInput::Snapshot, "buffer input not a snapshot");
    refused(R"pipe(
public composition Startup { singleton input CancellationToken; }
)pipe", "unsupported composition input type", "singleton input");
    refused(R"pipe(
public composition Startup { singleton input string; scoped input string; }
)pipe", "ambiguous composition binding", "scoped input");
    refused(R"pipe(
public source Interface IStore { pure int Read(); }
public Class Unrelated { public pure int Read() => 1; }
public composition Startup { transient IStore = Unrelated; }
)pipe", "not assignable", "transient IStore");
    refused(R"pipe(
public Interface IHost { pure int Read(); }
public Class Unrelated { public pure int Read() => 1; }
public composition Startup { transient IHost = Unrelated; }
)pipe", "not assignable", "transient IHost");
    refused(R"pipe(
public Interface IHost { pure int Read(); }
public composition Startup { singleton input IHost; export IHost; }
)pipe", "host interfaces are constructor-only", "export IHost");
    refused(R"pipe(
public Interface IHost { pure int Read(); }
public composition Startup { singleton IHost = IHost; }
)pipe", "ordinary source classes", "singleton IHost");
}

void constructorRefusals() {
    refused(R"pipe(
public Class Dependency {}
public Class Consumer { public Consumer(Dependency required) {} }
public composition Startup { transient Consumer = Consumer; }
)pipe", "missing composition binding", "required)");
    refused(R"pipe(
public Class Hidden { private Hidden() {} }
public composition Startup { transient Hidden = Hidden; }
)pipe", "no accessible public constructor", "transient Hidden");
    refused(R"pipe(
public Class Hidden { protected Hidden() {} }
public composition Startup { transient Hidden = Hidden; }
)pipe", "no accessible public constructor", "transient Hidden");
    refused(R"pipe(
public Class Choice { public Choice() {} public Choice(int value) {} }
public composition Startup { transient Choice = Choice; }
)pipe", "ambiguous public constructors", "transient Choice");
    refused(R"pipe(
public Class A { public A(B value) {} }
public Class B { public B(A value) {} }
public composition Startup { transient A = A; transient B = B; }
)pipe", "constructor dependency cycle");
    refused(R"pipe(
public Class AConsumer { public AConsumer(Z value) {} }
public Class Y { public Y(Z closing) {} }
public Class Z { public Z(Y value) {} }
public composition Startup {
    transient AConsumer = AConsumer;
    transient Y = Y;
    transient Z = Z;
}
)pipe", "Services.Z -> Services.Y -> Services.Z", "closing)");
    refused(R"pipe(
public Class A {}
public composition Startup { transient A = A; scoped A = A; }
)pipe", "ambiguous composition binding", "scoped A");
    refused(R"pipe(
public Class A {}
public composition Startup { transient input A; }
)pipe", "inputs require singleton or scoped", "transient input");

    const auto program = checked(R"pipe(
public Class Choice { private Choice() {} public Choice(int value) {} }
public composition Startup { singleton input int; transient Choice = Choice; }
)pipe");
    const auto graphs = planCompositions(program);
    require(serviceNamed(graphNamed(graphs, "Services.Startup"), "Services.Choice").dependencies.size() == 1,
            "private overload polluted public constructor selection");
}

void inclusionAndExports() {
    const auto program = checked(R"pipe(
public Class Clock {}
public Class UseCase { public UseCase(Clock clock) {} }
public composition Shared { singleton Clock = Clock; export Clock; }
public composition Left { include Shared; }
public composition Right { include Shared; }
public composition Startup { include Right; include Left; transient UseCase = UseCase; export UseCase; }
)pipe");
    const auto graphs = planCompositions(program);
    const auto& startup = graphNamed(graphs, "Services.Startup");
    require(graphs.size() == 4 && startup.services.size() == 2, "diamond inclusion duplicated registrations");
    require(startup.includes == std::vector<std::string>{"Services.Left", "Services.Right", "Services.Shared"}, "include closure not deterministic");
    require(startup.exports.size() == 1, "included exports leaked into root public surface");
    require(graphNamed(graphs, "Services.Left").exports.empty(), "no-export graph gained implicit exports");
    refused(R"pipe(
public Class Required {}
public Class Consumer { public Consumer(Required value) {} }
public composition Incomplete { transient Consumer = Consumer; }
public composition Startup { include Incomplete; singleton Required = Required; }
)pipe", "missing composition binding", "value)");
    refused(R"pipe(
public composition A { include B; }
public composition B { include A; }
)pipe", "include cycle");
    refused(R"pipe(
public composition Startup { include Missing; }
)pipe", "composition", "include Missing");
    refused(R"pipe(
public Class A {}
public composition Shared { singleton A = A; }
public composition Startup { include Shared; include Shared; }
)pipe", "duplicate", "include Shared; }");
    refused(R"pipe(
public Class A {}
public composition Startup { export A; }
)pipe", "export has no explicit binding", "export A");
    refused(R"pipe(
public Class A {}
public composition Startup { transient A = A; export A; export A; }
)pipe", "duplicate composition export", "export A; }");
}

void sourceFacetsAndImports() {
    const auto model = checked(R"pipe(
public source Interface IService { pure int Read(); }
public model Class Service : IService { public pure int Read() => 1; }
public composition Startup { transient IService = Service; transient Service = Service; export Service; }
)pipe");
    const auto graphs = planCompositions(model);
    const auto& graph = graphNamed(graphs, "Services.Startup");
    const auto& service = serviceNamed(graph, "Services.Service");
    const auto facet = std::find_if(model.declarations.begin(), model.declarations.end(), [&](const auto& type) {
        return type.name == service.implementation.name;
    });
    require(facet != model.declarations.end() && facet->generatedObjectFacet && !facet->modelExported,
            "composition selected an independent native model owner");
    require(serviceNamed(graph, "Services.IService").implementation.displayText() == "Services.Service", "model facet lost authored identity");

    std::vector<ModuleSource> modules{
        {"Contracts", "module Contracts; public source Interface IStore { pure int Read(); }", "contracts.pipe"},
        {"Data", "module Data; import Contracts; public Class Store : IStore { public pure int Read() => 1; }", "data.pipe"},
        {"App", "module App; import Contracts; import Data; public composition Startup { singleton IStore = Store; export IStore; }", "app.pipe"}};
    const auto first = compositionPlans(checked(modules, "App"));
    std::reverse(modules.begin(), modules.end());
    require(first == compositionPlans(checked(modules, "App")), "source input order changed graph plan");
    require(first[0]["services"][0]["service"] == "Contracts.IStore", "cross-module service identity lost");
}

void boundedDeepGraph() {
    std::string source;
    for (int index = 0; index < 96; ++index) {
        source += "public Class Service" + std::to_string(index) + " {";
        if (index > 0)
            source += "public Service" + std::to_string(index) + "(Service" + std::to_string(index - 1) + " previous) {}";
        source += "}\n";
    }
    source += "public composition Startup {\n";
    for (int index = 0; index < 96; ++index)
        source += "transient Service" + std::to_string(index) + " = Service" + std::to_string(index) + ";\n";
    source += "export Service95; }\n";
    const auto program = checked(source);
    const auto graphs = planCompositions(program);
    require(graphs[0].constructionOrder.size() == 96, "iterative dependency traversal lost a service");
    require(graphs[0].services[graphs[0].constructionOrder.front()].service.displayText() == "Services.Service0", "dependency order begins with consumer");
    ResourceBudget budget;
    budget.tokens = 4;
    const ScopedResourceBudget limited(budget);
    try {
        const auto refusedGraphs = planCompositions(program);
        static_cast<void>(refusedGraphs);
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find("configure tokens") != std::string::npos, "graph resource diagnostic is not actionable");
        require(diagnostic.path == "case.pipe", "budget diagnostic lost source origin");
        return;
    }
    throw std::runtime_error("composition work budget ignored");
}

}

int main() {
    try {
        typedInputsAndConstructors();
        lifetimeContexts();
        valuesAndNominalCompatibility();
        constructorRefusals();
        inclusionAndExports();
        sourceFacetsAndImports();
        boundedDeepGraph();
        std::cout << "composition planner assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "composition planner tests: " << error.what() << '\n';
        return 1;
    }
}
