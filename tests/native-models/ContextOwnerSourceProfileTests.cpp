// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace pipelang::models;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

void refused(const std::string& source, const std::string& expected) {
    try {
        compile(source, "context_owner_profile");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(expected) != std::string::npos,
            "unexpected context refusal: " + std::string(diagnostic.what()));
        require(diagnostic.offset < source.size(), "context diagnostic lost its authored token");
        return;
    }
    throw std::runtime_error("unsupported context owner source admitted: " + expected);
}

const std::string state = R"pipe(
public Class Model {
    private ContextOwner lifetime;
    public Model() { lifetime = ContextOwner.Create(); }
)pipe";

void admission() {
    const auto source = state + R"pipe(
    public pure ContextOwner Lifetime() => lifetime;
    public void Replace() { lifetime.Replace(); }
    public void Invalidate() { lifetime.Invalidate(); }
    public void Close() { lifetime.Close(); }
}
)pipe";
    const auto compiled = compile(source, "context_owner_profile");
    const auto header = compiled.at("header").get<std::string>();
    require(header.find("#include <pipelang/model/context.hpp>") != std::string::npos,
        "context runtime dependency is missing");
    require(header.find("std::shared_ptr<pipelang::model::ContextOwner> Lifetime() const") != std::string::npos,
        "framework getter lost its opaque owner identity");
    require(header.find(".createContextOwner()") != std::string::npos,
        "context construction did not create a runtime owner");
    require(header.find("(void)pl_receiver->replace()") != std::string::npos &&
        header.find("pl_receiver->invalidate()") != std::string::npos &&
        header.find("pl_receiver->close()") != std::string::npos,
        "source context operations lost their runtime implementation");
    const auto& model = compiled.at("manifest").at("classes").at(0);
    require(model.at("fields").empty(), "private context owner leaked into public snapshot fields");
    bool found = false;
    for (const auto& method : model.at("methods")) {
        if (method.at("name") == "Lifetime") {
            found = true;
            require(method.at("returnType") == "ContextOwner" && method.at("pure") == true &&
                method.at("parameters").empty(), "framework getter metadata was erased");
        }
    }
    require(found, "framework getter was omitted from ordinary method metadata");

    const auto modules = compileModules({{"Context", "module Context;\n" + source, "Context.pipe"}},
        "Context", "context_owner_profile");
    require(modules.at("ok") == true, "context owner builtin was lost during module composition");
    const auto objects = compileObjects(source, {"Model"}, "context_owner_profile");
    require(objects.at("ok") == true, "context owner rejected in exported object model");
    const auto project = compileProjectModules({{"Context", "module Context;\n" + source, "Context.pipe"}},
        "Context", "context_owner_profile");
    require(project.at("ok") == true, "context owner rejected in ordinary source object");
}

void refusals() {
    refused("public Class ContextOwner {}", "reserved type");
    refused("public Record Packet { public ContextOwner lifetime; }", "records contain owned value fields only");
    refused("public Class Model { public ContextOwner lifetime; }", "context owner fields must be private");
    refused(state + "public ContextOwner Lifetime() => lifetime; }", "public synchronous pure zero-argument getter");
    refused(state + "private pure ContextOwner Lifetime() => lifetime; }", "public synchronous pure zero-argument getter");
    refused(state + "public pure ContextOwner Lifetime(int value) => lifetime; }", "public synchronous pure zero-argument getter");
    refused(state + "public void Receive(ContextOwner incoming) {} }", "not method arguments");
    refused(state + "public void Reset() { lifetime = ContextOwner.Create(); } }", "identity is fixed by the constructor");
    refused(state + "public pure int Change() { lifetime.Invalidate(); return 1; } }", "requires an owner effect");
    refused(state + "public pure int Create() { ContextOwner local = ContextOwner.Create(); return 1; } }", "requires an owner effect");
    refused(state + "public void Unknown() { lifetime.Capture(); } }", "unknown context owner operation");
    refused(state + "public void Mix(string value) { lifetime = value; } }", "identity is fixed by the constructor");
    refused(state + "public void Mix() { string value = lifetime; } }", "expected string, got ContextOwner");
    refused(state + "public void Mix() { CancellationToken token = lifetime; } }", "local must be an owned value");
    refused(state + "public void Mix() { ContextOwner owner = \"request-id\"; } }", "expected ContextOwner, got string");
    refused(state + "public void Mix(CancellationToken cancellation) { ContextOwner owner = cancellation; } }", "expected ContextOwner, got CancellationToken");
    refused("public Class Model { private List<ContextOwner> values; }", "List requires an owned value payload");
    refused("public Class Model { private ContextOwner? value; }", "nullable suffix requires an owned value payload");
    refused("public Class Model { public pure Result<ContextOwner,string> Read() { return Result<ContextOwner,string>.Failure(\"x\"); } }",
        "Result payloads must recursively contain owned values");
    refused("public Class Model { private const ContextOwner value = ContextOwner.Create(); }", "const requires bool");
    refused(state + "public void Wrong() { lifetime.Replace(\"epoch\"); } }", "argument count mismatch");
    refused(state + "public pure bool Equal() => lifetime == lifetime; }", "equality requires owned values");
    refused(state + "public void Work(List<int> values) { parallel foreach (int value in values) { lifetime.Invalidate(); } } }",
        "context owners cannot enter parallel workers");
    refused(state + "private void Change() { lifetime.Invalidate(); } public void Work(List<int> values) { parallel foreach (int value in values) { Change(); } } }",
        "parallel helper cannot access context owners");
}
}

int main() {
    admission();
    refusals();
    std::cout << assertions << " context owner source assertions passed\n";
}
