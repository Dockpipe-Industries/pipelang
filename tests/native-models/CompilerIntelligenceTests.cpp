// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace pipelang::models;
using Json = nlohmann::json;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

const std::string library = R"pipe(module Library;
// café 😀: all metadata ranges use the original UTF-8 bytes.
public Record Payload {
    public int value;
}
public Record Box<T> {
    public T value;
}
public Record Unused<T> {
    public T value;
}
public Class Utility {
    public static pure int Scale(int left, int right) => left * 10 + right;
    public static pure int Format(int left, string right) => left;
    public static pure int Overload(int value) => value;
    public static pure string Overload(string value) => value;
    public static pure T Identity<T>(T value) => value;
    public static pure int At(int value) => value;
}
public Class Base {
    public virtual pure int Read() => 1;
}
public Class Derived : Base {
    public override pure int Read() => base.Read() + 1;
}
public source Interface IReader {
    pure int Read();
}
public Class Implementation : IReader {
    public pure int Read() => 3;
}
public Class ExplicitConstructor {
    public ExplicitConstructor(int value) {}
}
public Interface NativeHost {
    pure Result<int,string> Native();
}
)pipe";

const std::string entry = R"pipe(module Consumer;
import Library;
public model Class App {
    private NativeHost host;

    public App(NativeHost host) {
        this.host = host;
    }

    public pure Payload Echo(Payload parameter) {
        var copy = parameter;

        return copy;
    }

    public pure int Nested() {
        var boxed = Utility.Identity<Box<int>>(new Box<int> { value = 7 });
        var quoted = Utility.Format(1, "comma, ) < >");

        return Utility.Scale(1, (Utility.Scale(2, 3))) + Utility.Overload(4) +
            Utility.Identity<int>(5) + boxed.value + quoted;
    }

    public pure int Conditional() => Utility.Scale(1 < 2 ? 3 : 4, 5);

    public pure int Dispatch(Base receiver, IReader view) => receiver.Read() + view.Read();

    public pure Result<int,string> Observe() => host.Native();

    public pure int Builtin(List<int> values) => values.At(0);

    public int Construct() {
        var item = new ExplicitConstructor(4);

        return 1;
    }
}
)pipe";

std::vector<ModuleSource> modules() {
    return {{"Library", library, "lib/Library.pipe"}, {"Consumer", entry, "app/App.pipe"}};
}

const Json& methodNamed(const Json& analysis, const std::string& owner, const std::string& name,
                        const std::string& parameterType = {}) {
    const Json* result = nullptr;
    for (const auto& method : analysis.at("methods")) {
        if (method.at("owner") != owner || method.at("name") != name)
            continue;

        if (!parameterType.empty() && (method.at("parameters").empty() ||
            method.at("parameters").at(0).at("type") != parameterType))
            continue;

        require(result == nullptr, "method lookup is ambiguous: " + owner + '.' + name);
        result = &method;
    }
    require(result != nullptr, "missing checked method: " + owner + '.' + name);
    return *result;
}

const Json& typeNamed(const Json& analysis, const std::string& identity) {
    const auto& types = analysis.at("types");
    const auto found = std::find_if(types.begin(), types.end(), [&](const Json& type) {
        return type.at("id") == identity;
    });
    require(found != types.end(), "missing authored type: " + identity);
    return *found;
}

void signaturesAndTypes(const Json& analysis) {
    const auto& scale = methodNamed(analysis, "Library.Utility", "Scale");
    require(scale.at("parameters") == Json::array({{{"name", "left"}, {"type", "int"}},
        {{"name", "right"}, {"type", "int"}}}), "checked signature lost parameter names/types/order");
    require(scale.at("return_type") == "int" && scale.at("static") == true &&
        scale.at("pure") == true && scale.at("rename_safe") == true,
        "ordinary static method signature/effects/rename identity changed");
    require(methodNamed(analysis, "Library.Utility", "Overload", "int").at("rename_safe") == false,
        "overload rename was admitted without an overload-family contract");
    require(methodNamed(analysis, "Library.Utility", "Identity", "int").at("rename_safe") == false,
        "generic specialization rename was admitted as an isolated method");
    require(methodNamed(analysis, "Library.Base", "Read").at("rename_safe") == false &&
        methodNamed(analysis, "Library.Derived", "Read").at("rename_safe") == false,
        "virtual-family rename was admitted without the exact family contract");

    const auto& unused = typeNamed(analysis, "Library.Unused");
    require(unused.at("name") == "Unused" && unused.at("type_parameters") == Json::array({"T"}),
        "unused generic type declaration disappeared during closed normalization");
    const auto& payload = typeNamed(analysis, "Library.Payload");
    require(payload.at("offset") == library.find("Payload"), "type declaration range drifted");

    bool parameter = false;
    bool local = false;
    bool generic = false;
    for (const auto& symbol : analysis.at("symbols")) {
        if (symbol.at("path") != "app/App.pipe" || symbol.at("declaration") != true)
            continue;

        if (symbol.at("name") == "parameter") {
            require(symbol.at("type") == "Library.Payload" &&
                symbol.at("type_definition").at("id") == "Library.Payload",
                "parameter type definition lost the checked nominal identity");
            parameter = true;
        }
        if (symbol.at("name") == "copy") {
            require(symbol.at("type_definition").at("offset") == payload.at("offset"),
                "inferred local type definition lost its authored declaration");
            local = true;
        }
        if (symbol.at("name") == "boxed") {
            require(symbol.at("type") == "Library.Box<int>" &&
                symbol.at("type_definition").at("id") == "Library.Box",
                "closed generic nominal definition leaked a generated type name");
            generic = true;
        }
    }
    require(parameter && local && generic, "nominal local/parameter definition coverage is incomplete");
}

void exactCallGraph(const Json& analysis) {
    const std::map<std::string, std::string> sources{
        {"lib/Library.pipe", library}, {"app/App.pipe", entry}};
    std::set<std::string> methods;
    for (const auto& method : analysis.at("methods"))
        methods.insert(method.at("id").get<std::string>());

    bool nested = false;
    bool quoted = false;
    bool base = false;
    bool virtualCall = false;
    bool interfaceCall = false;
    bool constructor = false;
    std::size_t references = 0;
    for (const auto& call : analysis.at("calls")) {
        const auto path = call.at("path").get<std::string>();
        const auto& source = sources.at(path);
        const auto offset = call.at("offset").get<std::size_t>();
        const auto length = call.at("length").get<std::size_t>();
        require(source.substr(offset, length) == call.at("name").get<std::string>(),
            "call metadata lost the exact authored member token");
        require(methods.contains(call.at("callable").get<std::string>()) &&
            methods.contains(call.at("caller").get<std::string>()),
            "call graph has an unresolved callee or enclosing caller ID");
        require(call.at("definition").at("id") == call.at("callable"),
            "call definition does not name its checked callee");
        require(call.at("name") != "At" && call.at("name") != "Native",
            "unbound built-in/host member was guessed from an unrelated source method");
        for (const auto& target : call.at("targets"))
            require(methods.contains(target.get<std::string>()), "dispatch target lacks a checked source method");

        const auto open = call.at("open_paren").get<std::size_t>();
        const auto close = call.at("close_paren").get<std::size_t>();
        require(source.at(open) == '(' && source.at(close) == ')' && close > open,
            "signature help call extent lost its balanced source parentheses");
        require(call.at("arguments").size() == call.at("signature").at("parameters").size(),
            "signature help argument ranges disagree with the checked call arity");
        for (const auto& argument : call.at("arguments")) {
            const auto start = argument.at("offset").get<std::size_t>();
            const auto end = start + argument.at("length").get<std::size_t>();
            require(start > open && end <= close && end > start,
                "signature argument range escaped its checked call");
        }

        if (path == "app/App.pipe" && offset == entry.find("Scale(1, (")) {
            const auto& argument = call.at("arguments").at(1);
            const auto text = entry.substr(argument.at("offset").get<std::size_t>(),
                argument.at("length").get<std::size_t>());
            require(text == "(Utility.Scale(2, 3))", "nested call commas changed the outer argument range");
            nested = true;
        }
        if (path == "app/App.pipe" && call.at("name") == "Format") {
            const auto& argument = call.at("arguments").at(1);
            require(entry.substr(argument.at("offset").get<std::size_t>(),
                argument.at("length").get<std::size_t>()) == "\"comma, ) < >\"",
                "string punctuation changed signature-help argument boundaries");
            quoted = true;
        }
        if (path == "lib/Library.pipe" && offset == library.find("base.Read()") + 5)
            base = true;

        if (path == "app/App.pipe" && offset == entry.find("receiver.Read()") + 9) {
            require(call.at("targets").size() == 2, "virtual call lost its checked implementation union");
            virtualCall = true;
        }
        if (path == "app/App.pipe" && offset == entry.find("view.Read()") + 5) {
            require(call.at("targets").size() == 1 &&
                call.at("targets").at(0) != call.at("callable"),
                "source-interface call lost selected declaration versus concrete implementation");
            interfaceCall = true;
        }
        if (call.at("name") == "ExplicitConstructor")
            constructor = true;
    }
    for (const auto& symbol : analysis.at("symbols")) {
        if (symbol.at("kind") == "method" && symbol.at("declaration") == false) {
            require(symbol.contains("callable") && symbol.contains("definition"),
                "method reference lacks its exact compiler binding");
            ++references;
        }
    }
    require(nested && quoted && base && virtualCall && interfaceCall && constructor && references > 5,
        "call/signature/reference graph coverage is incomplete");
    require(methodNamed(analysis, "Library.IReader", "Read").at("implementations").size() == 1,
        "interface declaration lost its exact implementation locations");
}

void refusesUnknownMembers() {
    auto source = entry;
    const auto start = source.find("Utility.Overload(4)");
    source.replace(start, std::string("Utility.Overload(4)").size(), "Utility.Overload(true)");
    const std::vector<ModuleSource> sources{
        {"Library", library, "lib/Library.pipe"},
        {"Consumer", source, "app/App.pipe"}
    };

    const auto diagnosticFrom = [](const auto& run) {
        try {
            run();
        } catch (const Diagnostic& diagnostic) {
            return std::tuple{std::string(diagnostic.what()), diagnostic.offset,
                              diagnostic.module, diagnostic.path};
        }
        throw std::runtime_error("frontend accepted an invalid call binding");
    };

    const auto analysis = diagnosticFrom([&] {
        (void)analyzeProjectModules(sources, "Consumer");
    });
    require(std::get<0>(analysis).find("matching method overload") != std::string::npos,
        "invalid call binding lost its owning type-checker diagnostic");
    require(diagnosticFrom([&] {
        (void)compileProjectModules(sources, "Consumer", "intelligence_fixture");
    }) == analysis, "project bindings and editor analysis reported different diagnostics");
    require(diagnosticFrom([&] {
        (void)compileProjectImplementation(sources, "Consumer", "intelligence_fixture", "sources");
    }) == analysis, "compiled project and editor analysis reported different diagnostics");
}

void outputBudget() {
    auto low = resourceBudget();
    low.outputBytes = 512;
    ScopedResourceBudget scope(low);
    bool refused = false;
    try {
        (void)analyzeProjectModules(modules(), "Consumer");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find("output_bytes") != std::string::npos,
            "semantic metadata budget failure lost its resource diagnostic");
        refused = true;
    }
    require(refused, "enriched semantic metadata bypassed the configured output bound");
}
}

int main() {
    try {
        const auto analysis = analyzeProjectModules(modules(), "Consumer");
        require(analysis.at("ok") == true && analysis.at("diagnostics").empty(),
            "checked compiler intelligence analysis failed");
        signaturesAndTypes(analysis);
        exactCallGraph(analysis);
        refusesUnknownMembers();
        outputBudget();
        std::cout << assertions << " compiler intelligence assertions passed\n";
        return 0;
    } catch (const Diagnostic& diagnostic) {
        std::cerr << diagnostic.what() << " at byte " << diagnostic.offset << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
