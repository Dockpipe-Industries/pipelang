// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/CompositionFactoryProfile.hpp"
#include "../../src/native/model-compiler/CompositionPlanner.hpp"
#include "../../src/native/model-compiler/composition.hpp"

#include <iostream>
#include <initializer_list>
#include <stdexcept>

namespace {
using namespace pipelang::models;
std::size_t assertions = 0;

void require(bool value, const std::string& message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}

Program checked(const std::string& source) {
    auto program = linkModules({{"Services", "module Services;\n" + source, "Lifecycle.pipe"}}, "Services", true, true);
    program.objectEnabled = true;
    normalizeComposition(program);
    check(program);
    return program;
}

const std::string resourceTypes = R"pipe(
public source Interface ICleanup { Result<void,string> Close(CancellationToken token); }
public source Interface IService { int Read(); }
public Class Resource : IService, ICleanup {
    private int value;
    public Resource() { value = 1; }
    public int Read() => value;
    public Result<void,string> Close(CancellationToken token) { return Result<void,string>.Success(); }
}
)pipe";

void refused(const std::string& source, const std::string& message, bool profile = false) {
    try {
        auto program = checked(source);
        const auto graphs = planCompositions(program);
        if (profile)
            CompositionFactoryProfile(program, graphs).admit();
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(message) != std::string::npos,
            "wrong lifecycle refusal: " + std::string(diagnostic.what()));
        require(diagnostic.module == "Services" && diagnostic.path == "Lifecycle.pipe",
            "lifecycle diagnostic lost authored origin");
        return;
    }
    throw std::runtime_error("invalid lifecycle recipe accepted: " + message);
}

void typedRecipes() {
    auto program = checked(resourceTypes + R"pipe(
public Class ResourceFactory {
    public ResourceFactory() { }
    public Result<Resource,string> Create(CancellationToken token) {
        bool cleanupCancelled = CancellationToken.None.IsCancellationRequested;
        if (cleanupCancelled)
            return Result<Resource,string>.Cancelled("unexpected empty-token cancellation");
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton ResourceFactory = ResourceFactory;
    scoped IService = factory ResourceFactory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe");
    auto graphs = planCompositions(program);
    CompositionFactoryProfile(program, graphs).admit();
    require(graphs.size() == 1, "lifecycle graph missing");
    const auto json = compositionGraphJson(graphs.front());
    require(json.at("protocol") == "pipelang.composition-graph.v2", "lifecycle graph version not explicit");
    require(json.at("cleanup_contracts").size() == 1, "nominal cleanup contract lost");
    bool found = false;
    for (const auto& service : json.at("services")) {
        if (service.at("service") != "Services.IService")
            continue;
        found = true;
        require(service.at("constructor") == "" && service.at("constructor_origin").at("path") == "",
            "factory incorrectly claims a constructor");
        require(service.at("factory").at("receiver") == "Services.ResourceFactory", "factory receiver identity lost");
        require(service.at("dependencies").at(0).at("parameter") == "$factoryReceiver", "factory receiver not first dependency");
        require(service.at("factory").at("parameters").at(0).at("index").is_null(), "token became a service dependency");
        require(service.at("cleanup_index") == 0, "owned resource lacks selected cleanup");
    }
    require(found, "resource binding lost");
}

void invalidRecipes() {
    refused(resourceTypes + R"pipe(
public Class ResourceFactory { public ResourceFactory() {} public Resource Create() { return new Resource(); } }
public composition Startup { singleton ResourceFactory = ResourceFactory; scoped IService = factory ResourceFactory.Create; }
)pipe", "Result<ConcreteSourceClass,string>");
    refused(resourceTypes + R"pipe(
public Class ResourceFactory { public ResourceFactory() {} public Result<IService,string> Create() { return Result<IService,string>.Success(new Resource()); } }
public composition Startup { singleton ResourceFactory = ResourceFactory; scoped IService = factory ResourceFactory.Create; }
)pipe", "constructed composition implementations must be ordinary source classes");
    refused(resourceTypes + R"pipe(
public Class ResourceFactory { public ResourceFactory() {} public Result<Resource,string> Create() { return Result<Resource,string>.Success(new Resource()); } }
public composition Startup { scoped IService = factory ResourceFactory.Create; }
)pipe", "factory receiver requires an explicit");
    refused(resourceTypes + R"pipe(
public Class ResourceFactory {
    public ResourceFactory() {}
    public Result<Resource,string> Create(CancellationToken first, CancellationToken second) { return Result<Resource,string>.Success(new Resource()); }
}
public composition Startup { singleton ResourceFactory = ResourceFactory; scoped IService = factory ResourceFactory.Create; }
)pipe", "at most one CancellationToken");
    refused(resourceTypes + R"pipe(
public composition Startup { singleton Resource = Resource; cleanup Resource.Close; }
)pipe", "exact nominal source interface");
    refused(R"pipe(
public source Interface ICleanup { Result<void,string> Close(); }
public Class Resource : ICleanup { public Resource() {} public Result<void,string> Close() { return Result<void,string>.Success(); } }
public composition Startup { singleton Resource = Resource; cleanup ICleanup.Close; }
)pipe", "Method(CancellationToken)");
    auto multiple = resourceTypes;
    const auto implementation = multiple.find("Class Resource : IService, ICleanup");
    multiple.replace(implementation, std::string("Class Resource : IService, ICleanup").size(),
        "Class Resource : IService, ICleanup, ISecondCleanup");
    refused("public source Interface ISecondCleanup { Result<void,string> Close(CancellationToken token); }\n" + multiple + R"pipe(
public composition Startup { singleton Resource = Resource; cleanup ICleanup.Close; cleanup ISecondCleanup.Close; }
)pipe", "multiple cleanup contracts apply");
}

void factoryEscapeRefusals() {
    const std::string types = resourceTypes + R"pipe(
public Class Request { public Request() {} }
public Class ResourceFactory {
    private Request captured;
    public ResourceFactory() { captured = new Request(); }
    public Result<Resource,string> Create(Request request) {
        captured = request;
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton ResourceFactory = ResourceFactory;
    scoped Request = Request;
    scoped IService = factory ResourceFactory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe";
    refused(types, "cannot mutate existing source-object fields", true);
    auto helper = types;
    const auto assignment = helper.find("captured = request;");
    helper.replace(assignment, std::string("captured = request;").size(), "Capture(request);");
    const auto method = helper.find("public Result<Resource,string> Create");
    helper.insert(method, "private void Capture(Request request) { captured = request; }\n");
    refused(helper, "cannot mutate existing source-object fields", true);

    auto stateless = types;
    const auto directAssignment = stateless.find("captured = request;");
    stateless.erase(directAssignment, std::string("captured = request;").size());
    auto program = checked(stateless);
    const auto graphs = planCompositions(program);
    CompositionFactoryProfile(program, graphs).admit();
    require(graphs.front().services.size() == 3,
        "stateless singleton factory could not consume a scoped call argument");
}

void legacyGraphStable() {
    const auto graphs = planCompositions(checked(R"pipe(
public Class Service { public Service() {} }
public composition Startup { singleton Service = Service; export Service; }
)pipe"));
    const auto json = compositionGraphJson(graphs.front());
    require(json.at("protocol") == "pipelang.composition-graph.v1" && !json.contains("cleanup_contracts"),
        "legacy graph metadata unexpectedly upgraded");
    require(!json.at("services").at(0).contains("factory") && !json.at("services").at(0).contains("cleanup_index"),
        "legacy service metadata unexpectedly changed");
}

void factorySubtypeContracts() {
    refused(R"pipe(
public source Interface ICleanup { Result<void,string> Close(CancellationToken token); }
public Class Base { public Base() {} }
public Class Derived : Base, ICleanup {
    public Derived() : base() {}
    public Result<void,string> Close(CancellationToken token) { return Result<void,string>.Success(); }
}
public Class Factory {
    public Factory() {}
    public Result<Base,string> Create() { return Result<Base,string>.Success(new Base()); }
}
public composition Startup { singleton Factory = Factory; scoped Base = factory Factory.Create; cleanup ICleanup.Close; export Base; }
)pipe", "cleanup not exposed by declared return class", true);
    refused(R"pipe(
public source Interface ICleanup { Result<void,string> Close(CancellationToken token); }
public source Interface ISecondCleanup { Result<void,string> Close(CancellationToken token); }
public Class Base : ICleanup {
    public Base() {}
    public Result<void,string> Close(CancellationToken token) { return Result<void,string>.Success(); }
}
public Class Derived : Base, ISecondCleanup { public Derived() : base() {} }
public Class Factory {
    public Factory() {}
    public Result<Base,string> Create() { return Result<Base,string>.Success(new Base()); }
}
public composition Startup { singleton Factory = Factory; scoped Base = factory Factory.Create; cleanup ICleanup.Close; cleanup ISecondCleanup.Close; export Base; }
)pipe", "matches multiple selected cleanup contracts", true);
}

void factorySharedMutation() {
    refused(resourceTypes + R"pipe(
public Class SharedHolder {
    private Atomic<int> counter;
    public SharedHolder() { counter = Atomic<int>.Create(0); }
    public int Next() => counter.Add(1);
}
public Class Factory {
    private SharedHolder holder;
    public Factory(SharedHolder shared) { holder = shared; }
    public Result<Resource,string> Create() {
        int next = holder.Next();
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton SharedHolder = SharedHolder;
    singleton Factory = Factory;
    scoped IService = factory Factory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe", "resource factory nontransactional shared-handle mutation/wait/release", true);
}

void checkedFactoryDispatch() {
    refused(resourceTypes + R"pipe(
public Class Request { public Request() {} }
public source Interface IFactory { Result<Resource,string> Create(Request value); }
public Class Factory : IFactory {
    private Request captured;
    public Factory() { captured = new Request(); }
    public Result<Resource,string> Create(Request value) {
        captured = value;
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton IFactory = Factory;
    scoped Request = Request;
    scoped IService = factory IFactory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe", "cannot mutate existing source-object fields", true);
    refused(resourceTypes + R"pipe(
public Class SharedHolder {
    private Atomic<int> counter;
    public SharedHolder() { counter = Atomic<int>.Create(0); }
    public int Next() => counter.Add(1);
}
public Class BaseFactory {
    public BaseFactory() {}
    public virtual Result<Resource,string> Create() { return Result<Resource,string>.Success(new Resource()); }
}
public Class Factory : BaseFactory {
    private SharedHolder holder;
    public Factory(SharedHolder shared) : base() { holder = shared; }
    public override Result<Resource,string> Create() {
        int next = holder.Next();
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton SharedHolder = SharedHolder;
    singleton BaseFactory = Factory;
    scoped IService = factory BaseFactory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe", "resource factory nontransactional shared-handle mutation/wait/release", true);
}

void forHeaderFactoryWrites() {
    for (const auto& loop : {
            "for (captured = value; false;) { }",
            "for (int index = 0; index < 1; captured = value) { index = index + 1; }"}) {
        refused(resourceTypes + R"pipe(
public Class Request { public Request() { } }
public Class Factory {
    private Request captured;
    public Factory() { captured = new Request(); }
    public Result<Resource,string> Create(Request value) {
)pipe" + loop + R"pipe(
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton Factory = Factory;
    scoped Request = Request;
    scoped IService = factory Factory.Create;
    cleanup ICleanup.Close;
    export IService;
}
)pipe", "cannot mutate existing source-object fields", true);
    }
}

void unsupportedIndexedAssignment() {
    refused(R"pipe(
public Class Service {
    public Service() {}
    public int Read() {
        List<int> values = List<int>.Empty();
        values.At(0) = 1;
        return 0;
    }
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "invalid assignment target");
}
}

int main() {
    try {
        typedRecipes();
        invalidRecipes();
        factoryEscapeRefusals();
        factorySubtypeContracts();
        factorySharedMutation();
        checkedFactoryDispatch();
        forHeaderFactoryWrites();
        unsupportedIndexedAssignment();
        legacyGraphStable();
        std::cout << "composition lifecycle assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "composition lifecycle tests: " << error.what() << '\n';
        return 1;
    }
}
