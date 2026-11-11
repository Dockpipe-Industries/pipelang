// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pipelang::models;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

nlohmann::json factory(const std::string& source) {
    return compileProjectCompositionFactory({{"Services", "module Services;\n" + source, "Startup.pipe"}},
        "Services", "factory_profile");
}

void refused(const std::string& source, const std::string& cause) {
    try {
        factory(source);
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(cause) != std::string::npos,
                "unexpected factory refusal: " + std::string(diagnostic.what()));
        require(diagnostic.module == "Services" && diagnostic.path == "Startup.pipe",
                "factory refusal lost authored source identity");
        return;
    }
    throw std::runtime_error("unsupported factory profile was admitted: " + cause);
}

void admittedMethods() {
    const auto response = factory(R"pipe(
public Class Service {
    public Service() { }
    public int Echo(int context, int execution, int outcome) => context + execution + outcome;
    public Result<int,string> Read(int context, CancellationToken cancellation) {
        if (cancellation.IsCancellationRequested)
            return Result<int,string>.Cancelled("cancelled by source");
        return Result<int,string>.Success(context);
    }
    public void Touch() { }
}
public composition Startup {
    singleton Service = Service;
    export Service;
}
)pipe");
    const auto header = response.at("composition_factory_header").get<std::string>();
    require(response.at("ok") == true, "valid source factory refused");
    require(header.find("ServiceHandle") != std::string::npos && header.find("ResolveService") != std::string::npos,
            "typed export facade missing");
    require(header.find("pl_argument_0") != std::string::npos && header.find("auto pl_context") != std::string::npos,
            "authored method parameters were not isolated from internal native names");
    require(header.find("Result<pipelang::model::Result<") == std::string::npos,
            "public source Result facade unexpectedly nested its error boundary");
    require(header.find("CancellationToken pl_argument_1") != std::string::npos,
            "direct source cancellation parameter not forwarded");
    require(header.find("pl_cancellation.is_cancelled()") != std::string::npos,
            "constructor miss lacks final cancellation check before commit");
    require(header.find("SourceConstructor<") != std::string::npos && header.find("SourceMethods<") != std::string::npos,
            "checked manual-construction/method bridge missing");
    require(header.find("Root(const Root&) = delete") != std::string::npos &&
            header.find("Scope(Scope&&) = delete") != std::string::npos,
            "native owner/context cache identity can be duplicated");
    require(header.find("composition handle is empty") != std::string::npos,
            "moved-from handles lack a fallible invocation guard");
}

void validConstructionNamespace() {
    const auto response = compileProjectCompositionFactory({{"construction", R"pipe(module construction;
public Class Service { public Service() {} public int Read() => 1; }
public composition Startup { singleton Service = Service; export Service; }
)pipe", "Startup.pipe"}}, "construction", "factory_profile");
    const auto header = response.at("composition_factory_header").get<std::string>();
    require(header.find("::factory_profile::construction::Construct<") != std::string::npos &&
            header.find("::factory_profile::construction::SourceMethods<") != std::string::npos,
            "valid source namespace shadows its checked native bridge");
}

void rejectedSignaturesAndInputs() {
    const auto asynchronous = factory(R"pipe(
public Class Service {
    public Service() { }
    public async Task<Result<int,string>> Read() { return Result<int,string>.Success(1); }
}
public composition Startup { singleton Service = Service; export Service; }
)pipe");
    require(asynchronous.at("composition_factory_header").get<std::string>().find("->start<") != std::string::npos,
            "scoped async facade did not use the owning task admission boundary");
    refused(R"pipe(
public Class Service {
    public Service() { }
    public Service Self() { return this; }
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "nongeneric owned-value");
    refused(R"pipe(
public Class Service { public Service() { } public int Read() => 1; }
public composition Startup { singleton input Service; export Service; }
)pipe", "borrowed source references");
    refused(R"pipe(
public Enum Failure { Broken }
public Class Service {
    public Service() { }
    public Result<int,Failure> Read() { return Result<int,Failure>.Failure(Failure.Broken); }
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "typed nonstring Result errors");
}

void constructorEffects() {
    refused(R"pipe(
public Interface Host { Result<int,string> Read(); }
public Class Service {
    private Host source;
    public Service(Host input) { source = input; int value = Initialize(); }
    private int Initialize() {
        Result<int,string> read = source.Read();
        return read.IsSuccess ? read.Value : 0;
    }
    public int Value() => 1;
}
public composition Startup {
    singleton input Host;
    singleton Service = Service;
    export Service;
}
)pipe", "transitive pure owned host reads");
    refused(R"pipe(
public Interface Host { Result<int,string> Read(); }
public Class Base {
    public Base(Host input) { Result<int,string> value = input.Read(); }
}
public Class Service : Base {
    public Service(Host input) : base(input) { }
    public int Value() => 1;
}
public composition Startup {
    singleton input Host;
    singleton Service = Service;
    export Service;
}
)pipe", "transitive pure owned host reads");
    const auto response = factory(R"pipe(
public Interface Host { pure Result<int,string> Read(); }
public Class Service {
    public Service(Host input) {
        Result<int,string> read = input.Read();
        if (!read.IsSuccess) throw Error(read.Error);
    }
    public int Value() => 1;
}
public composition Startup {
    singleton input Host;
    singleton Service = Service;
    export Service;
}
)pipe");
    require(response.at("ok") == true, "pure owned Result constructor read was refused");
}

void emittedNameConflicts() {
    refused(R"pipe(
public Class Service {
    public Service() { }
    public int Read(int value) => value;
    public int Read(int value, CancellationToken cancellation) => value;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "facade cancellation boundary");
    refused(R"pipe(
public Class Service { public Service() { } public int ServiceHandle() => 1; }
public composition Startup { singleton Service = Service; export Service; }
)pipe", "generated handle/bridge type");
    refused(R"pipe(
public Class Service { public Service() { } public int SourceMethods() => 1; }
public composition Startup { singleton Service = Service; export Service; }
)pipe", "generated handle/bridge type");
    refused(R"pipe(
public Class Service { public Service() { } public int Read() => 1; }
public composition Root { singleton Service = Service; export Service; }
)pipe", "conflicts with a native facade");
    refused(R"pipe(
public Class Service { public Service() { } public int Read() => 1; }
public composition ServiceHandle { singleton Service = Service; export Service; }
)pipe", "export handle name conflicts");
}

void forHeaderConstructionEffects() {
    for (const auto& loop : {
            "for (int index = counter.Add(1); index < 2; index = index + 1) { }",
            "for (int index = 0; index < 1; counter.Add(1)) { index = index + 1; }"}) {
        refused(std::string(R"pipe(
public Class Service {
    public Service() {
        Atomic<int> counter = Atomic<int>.Create(0);
)pipe") + loop + R"pipe(
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "nontransactional shared-handle mutation/wait/release");
    }
}

void nontransactionalConstructionEffects() {
    const std::string source = R"pipe(
public Class SharedHolder {
    private Atomic<int> cell;
    public SharedHolder() { cell = Atomic<int>.Create(0); }
    public int Next() => cell.Add(1);
    public int Read() => cell.Load();
}
public Class Service {
    public Service(SharedHolder holder) { int value = holder.Next(); throw Error("failed"); }
    public int Read() => 1;
}
public composition Startup {
    singleton SharedHolder = SharedHolder;
    singleton Service = Service;
    export SharedHolder;
    export Service;
}
)pipe";
    bool observed = false;
    try {
        factory(source);
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find("nontransactional shared-handle mutation/wait/release") != std::string::npos,
                "shared singleton mutation escaped constructor effect admission");
        require(diagnostic.module == "Services" && diagnostic.path == "Startup.pipe" &&
                diagnostic.offset == ("module Services;\n" + source).find("cell.Add(1)"),
                "shared mutation refusal lost the exact transitive effect origin");
        observed = true;
    }
    require(observed, "nontransactional shared singleton mutation admitted");
    refused(R"pipe(
public Class Service {
    public Service() {
        ConcurrentQueue<int> queue = ConcurrentQueue<int>.Empty();
        queue.Enqueue(1);
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "nontransactional shared-handle mutation/wait/release");
    refused(R"pipe(
public Class Service {
    public Service() {
        SemaphoreSlim gate = SemaphoreSlim.Create(1, 1);
        gate.Wait();
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "nontransactional shared-handle mutation/wait/release");
    refused(R"pipe(
public Class Service {
    public Service() {
        SemaphoreSlim gate = SemaphoreSlim.Create(0, 1);
        gate.Release();
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "nontransactional shared-handle mutation/wait/release");
    refused(R"pipe(
public Class Service {
    public Service() {
        Lock gate = Lock.Create();
        lock (gate) { }
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "constructor synchronization/parallel work");
    refused(R"pipe(
public Class Service {
    public Service() {
        List<int> values = List<int>.Empty();
        parallel foreach (int value in values) { int doubled = value + value; }
    }
    public int Read() => 1;
}
public composition Startup { singleton Service = Service; export Service; }
)pipe", "constructor synchronization/parallel work");
}
}

int main() {
    try {
        admittedMethods();
        validConstructionNamespace();
        rejectedSignaturesAndInputs();
        constructorEffects();
        emittedNameConflicts();
        nontransactionalConstructionEffects();
        forHeaderConstructionEffects();
        std::cout << "composition factory profile assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "composition factory profile tests: " << error.what() << '\n';
        return 1;
    }
}
