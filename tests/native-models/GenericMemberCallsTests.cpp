// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using namespace pipelang::models;
using Json = nlohmann::json;
std::size_t assertions = 0;

void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    require(output.good(), "cannot write " + path.string());
}

Json accept(const std::string& source) {
    return compileObjects(source + " public Class App {}", {"App"}, "member_oracle");
}

void refuse(const std::string& source, const std::string& expected) {
    try {
        (void)accept(source);
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find(expected) != std::string::npos,
            "expected " + expected + ", got " + diagnostic.what());
        require(diagnostic.offset < source.size(), "member diagnostic lost the source offset");
        return;
    }
    throw std::runtime_error("accepted invalid member application: " + expected);
}

const std::string equality = R"pipe(
public Class Assertions {
    public pure bool Equal<T>(T expected, T actual, string message) => expected == actual;
}
)pipe";

void contracts() {
    require(accept(equality + R"pipe(
public Class Use {
    public pure bool Check(Assertions assertions) => assertions.Equal<int>(1, 1, "int");
}
)pipe").at("ok") == true, "typed parameter receiver was rejected");

    require(accept(R"pipe(
public Class Base {
    protected pure T Echo<T>(T value) => value;
}
public Class Derived : Base {
    public pure int Check() => this.Echo<int>(3) + base.Echo<int>(4);
}
)pipe").at("ok") == true, "protected/inherited generic member receiver was rejected");

    require(accept(R"pipe(
public Class Left { public pure T Identity<T>(T value) => value; }
public Class Right { public pure T Identity<T>(T value) => value; }
public Class Use {
    public pure int Check(Left left, Right right) {
        return left.Identity<int>(3) + right.Identity<int>(4);
    }
}
)pipe").at("ok") == true, "same-name generic members in unrelated classes collided");

    require(accept(R"pipe(
public Class Base { public pure T Identity<T>(T value) => value; }
public Class Derived : Base { public pure T Identity<T>(T value, int ignored) => value; }
public Class Use {
    public pure int Check(Derived receiver) {
        return receiver.Identity<int>(3) + receiver.Identity<int>(4, 0);
    }
}
)pipe").at("ok") == true, "inherited generic overload set was not preserved");

    refuse(equality + R"pipe(
public Class Use {
    public pure bool Check(Assertions assertions) => assertions.Equal<int>(1, "wrong", "type");
}
)pipe", "no exact matching method overload for 'Equal<int>'");
    refuse(equality + R"pipe(
public Class Use {
    public pure bool Check(Assertions assertions) => assertions.Equal<int>(1, 1);
}
)pipe", "no exact matching method overload");
    refuse(equality + R"pipe(
public Class Use {
    public pure bool Check(Assertions assertions) => assertions.Equal<int,string>(1, 1, "arity");
}
)pipe", "explicit generic argument count mismatch");
    refuse(equality + R"pipe(
public Class Use {
    public pure bool Check(Assertions assertions) => assertions.Missing<int>(1);
}
)pipe", "unknown generic method");
    refuse(R"pipe(
public Class Secret { private pure T Echo<T>(T value) => value; }
public Class Use { public pure int Check(Secret secret) => secret.Echo<int>(1); }
)pipe", "private object method");
    refuse(R"pipe(
public Class Base { protected pure T Echo<T>(T value) => value; }
public Class Derived : Base {}
public Class Use { public pure int Check(Derived other) => other.Echo<int>(1); }
)pipe", "private object method");
    refuse(R"pipe(
public Class Effects { public T Echo<T>(T value) => value; }
public Class Use { public pure int Check(Effects effect) => effect.Echo<int>(1); }
)pipe", "pure methods cannot call object effects");
    refuse(R"pipe(
public Class Use { public pure int Check(int value) => value.Echo<int>(1); }
)pipe", "concrete source class receiver");
    refuse(R"pipe(
public source Interface Contract { pure int Echo(int value); }
public Class Impl : Contract { public pure int Echo(int value) => value; }
public Class Use { public pure int Check(Contract value) => value.Echo<int>(1); }
)pipe", "concrete source class receiver");
    refuse(R"pipe(
public Class Virtual { public virtual pure T Echo<T>(T value) => value; }
)pipe", "generic virtual methods");
    refuse(R"pipe(
public Class Base { public pure T Echo<T>(T value) => value; }
public Class Derived : Base {
    public pure T Echo<T>(T value) => value;
    public pure int Check() => this.Echo<int>(1);
}
)pipe", "explicit override");
    refuse(R"pipe(
public Class Overloaded {
    public pure T Echo<T>(T value) => value;
    public pure int Echo<T>(int value) => value;
}
public Class Use { public pure int Check(Overloaded other) => other.Echo<int>(1); }
)pipe", "duplicate method signature");
    refuse(R"pipe(
public Class Local {
    private pure T Loop<T>(T value) => Loop<List<T>>(List<T>.Empty()).At(0);
    public pure int Check() => Loop<int>(1);
}
)pipe", "expanding recursive generic method instantiation");
    require(accept(R"pipe(
public Class Local {
    private pure T Loop<T>(T value) => Loop<T>(value);
    public pure int Check() => Loop<int>(1);
}
)pipe").at("ok") == true, "closed generic recursion must use the source call budget");
    refuse(R"pipe(
public Class Local {
    private pure T Identity<T>(T value) => value;
    public pure int Check() => Identity<int>("wrong");
}
)pipe", "no exact matching method overload");
    require(accept(R"pipe(
public Class Local {
    private pure T Identity<T>(T value) => value;
    public pure int Check() => Identity<int>(3);
}
)pipe").at("ok") == true, "local generic admission changed");

    // Generic records still obey the configured closed-instance policy. Member
    // specialization must not bypass admission simply because the receiver is typed.
    auto low = resourceBudget();
    low.genericInstances = 1;
    ScopedResourceBudget budget(low);
    refuse(R"pipe(
public Record Box<T> { public T value; }
public Class Wrap { public pure Box<T> Make<T>(T value) => new Box<T> { value = value }; }
public Class Use {
    public void Check(Wrap receiver) {
        Box<int> first = receiver.Make<int>(1);
        Box<string> second = receiver.Make<string>("two");
    }
}
)pipe", "closed generic type limit");
}


void staticContracts() {
    require(accept(R"pipe(
public Class Utility {
    private int state = 42;
    public Utility() { throw Error("must not construct"); }
    private static pure T Echo<T>(T value) => value;
    public static pure int Answer() => Echo<int>(42);
}
public Class Use { public pure int Read() => Utility.Answer(); }
)pipe").at("ok") == true, "receiver-free static generic helper was rejected");

    require(accept(R"pipe(
public Class Base { protected static pure T Echo<T>(T value) => value; }
public Class Derived : Base { public static pure int Read() => Derived.Echo<int>(7); }
)pipe").at("ok") == true, "inherited protected static generic member was rejected");

    const std::string staticFactory = R"pipe(module StaticFactory;
public Class Resource {}
public Class Factory {
    public static Result<Resource,string> Create() {
        return Result<Resource,string>.Success(new Resource());
    }
}
public composition Startup {
    singleton Factory = Factory;
    scoped Resource = factory Factory.Create;
    export Resource;
})pipe";
    bool factoryRefused = false;
    try {
        compileProjectCompositionFactory({{"StaticFactory", staticFactory, "StaticFactory.pipe"}},
            "StaticFactory", "static_factory");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find("composition method selector") != std::string::npos,
            "static instance-factory refusal lost its owning diagnostic");
        factoryRefused = true;
    }
    require(factoryRefused, "static method was admitted as an instance composition factory");

    const std::string staticExport = R"pipe(module StaticExport;
public Class Service {
    public static pure int StaticOnly() => 1;
    public pure int Instance() => StaticOnly();
}
public composition Startup {
    singleton Service = Service;
    export Service;
})pipe";
    const auto exported = compileProjectCompositionFactory({{"StaticExport", staticExport, "StaticExport.pipe"}},
        "StaticExport", "static_export");
    require(exported.at("composition_factory_header").get<std::string>().find(" StaticOnly(") == std::string::npos,
        "static method was exported as an instance composition action");

    const std::vector<std::pair<std::string, std::string>> invalid{
        {R"pipe(public Class Utility { public static pure int Read() => 1; }
        public Class Use { public pure int Read(Utility Utility) => Utility.Read(); })pipe", "type-qualified call"},
        {R"pipe(public Class Utility { public static pure T Read<T>(T value) => value; }
        public Class Use {
            private Utility Utility;
            public Use() { Utility = new Utility(); }
            public pure int Read() => Utility.Read<int>(1);
        })pipe", "type-qualified call"},
        {R"pipe(public Class Utility {
            private int value = 1;
            public static pure int Read() => value;
        })pipe", "static methods cannot access instance fields"},
        {R"pipe(public Class Utility {
            private int value = 1;
            public static void Write() { value = 2; }
        })pipe", "static methods cannot access instance fields"},
        {R"pipe(public Class Utility {
            public static pure Utility Self() => this;
        })pipe", "static methods cannot access this"},
        {R"pipe(public Class Base { public pure int Read() => 1; }
        public Class Utility : Base {
            public static pure int ReadOther() => base.Read();
        })pipe", "static methods cannot access base"},
        {R"pipe(public Class Utility {
            private pure int Read() => 1;
            public static pure int Value() => Read();
        })pipe", "cannot call instance methods"},
        {R"pipe(public Class Utility {
            private static pure int Read() => 1;
        }
        public Class Use { public pure int Read() => Utility.Read(); })pipe", "private static method"},
        {R"pipe(public Class Utility {
            private static pure T Read<T>(T value) => value;
        }
        public Class Use { public pure int Read() => Utility.Read<int>(1); })pipe", "private static method"},
        {R"pipe(public Class Utility { public static pure int Read() => 1; }
        public Class Use { public pure int Read(Utility value) => value.Read(); })pipe", "type-qualified call"},
        {R"pipe(public Class Utility { public static pure T Read<T>(T value) => value; }
        public Class Use { public pure int Read(Utility value) => value.Read<int>(1); })pipe", "type-qualified call"},
        {R"pipe(public Class Utility { public static int Read() => 1; }
        public Class Use { public pure int Read() => Utility.Read(); })pipe", "pure methods cannot call static effects"},
        {R"pipe(public Class Utility { public static pure T Read<T>(T value) => value; }
        public Class Use { public pure int Read() => Utility.Read<int>("bad"); })pipe", "no exact matching method overload"},
        {R"pipe(public Class Utility { public static pure T Read<T>(T value) => value; }
        public Class Use { public pure int Read() => Utility.Read<int,string>(1); })pipe", "explicit generic argument count mismatch"},
        {R"pipe(public Class Utility { public static Utility() {} })pipe", "static methods require"},
        {R"pipe(public Class Utility { public static virtual pure int Read() => 1; })pipe", "static methods require"},
        {R"pipe(public source Interface Contract { static pure int Read(); })pipe", "static methods require"},
        {R"pipe(public source Interface Contract { pure int Read(); }
        public Class Utility : Contract { public static pure int Read() => 1; })pipe", "exact source interface contract"},
        {R"pipe(public Class Utility {
            public static async Task<Result<int,string>> Read() {
                return Result<int,string>.Success(1);
            }
        }
        public Class Use { public void Read() { Utility.Read(); } })pipe", "async static calls must be directly awaited"}
    };
    for (const auto& [source, diagnostic] : invalid)
        refuse(source, diagnostic);
}

void emitStatic(const std::filesystem::path& directory, const Json& result) {
    std::filesystem::create_directories(directory);
    write(directory / "Model.hpp", result.at("header").get<std::string>());
    if (result.contains("types_header"))
        write(directory / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("constants_header"))
        write(directory / "Constants.hpp", result.at("constants_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "invalid static implementation path");
            write(directory / name, file.at("content").get<std::string>());
        }
    }
}

void staticFixtures(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto assertions = read(root / "sdk/libraries/testing/src/Assert.pipe");
    const auto compatibility = read(root / "sdk/libraries/testing/src/Assertions.pipe");
    const auto failure = read(root / "sdk/libraries/testing/src/TestFailure.pipe");
    auto fixture = read(root / "tests/native-models/StaticMethods.pipe");
    const auto body = [](const std::string& fragment) {
        return fragment.substr(fragment.find(';') + 1);
    };
    const auto objectSource = body(assertions) + body(compatibility) + body(failure) + fixture;
    emitStatic(output / "static-objects", compileObjects(objectSource, {"App"}, "static_methods"));

    const std::string marker = "public Class App";
    const auto owner = fixture.find(marker);
    require(owner != std::string::npos, "static fixture root is missing");
    fixture.replace(owner, marker.size(), "public model Class App");
    fixture = "module StaticMethods;\nimport PipeLang.Testing;\n" + fixture;

    const std::vector<ModuleSource> modules{
        {"PipeLang.Testing", assertions, "sdk/libraries/testing/src/Assert.pipe"},
        {"PipeLang.Testing", compatibility, "sdk/libraries/testing/src/Assertions.pipe"},
        {"PipeLang.Testing", failure, "sdk/libraries/testing/src/TestFailure.pipe"},
        {"StaticMethods", fixture, "StaticMethods.pipe"}};
    const auto header = compileProjectModules(modules, "StaticMethods", "static_methods");
    const auto compiled = compileProjectImplementation(modules, "StaticMethods", "static_methods", "sources");
    require(header.at("manifest") == compiled.at("manifest"), "static compiled and header contracts diverged");
    emitStatic(output / "static-project", header);
    emitStatic(output / "static-compiled", compiled);

    const std::string entry = R"pipe(module StaticAssertConsumer;
import PipeLang.Testing;
public model Class App {
    public pure Result<void,TestFailure> Check(bool value) => Assert.True(value, "message");
    public pure Result<void,TestFailure> Equal() => Assert.Equal<int>(42, 42, "equal");
    public pure Result<void,TestFailure> Compatible(Assertions value) => value.True(true, "compatible");
})pipe";
    const std::vector<ModuleSource> testing{
        {"PipeLang.Testing", assertions, "sdk/libraries/testing/src/Assert.pipe"},
        {"PipeLang.Testing", compatibility, "sdk/libraries/testing/src/Assertions.pipe"},
        {"PipeLang.Testing", failure, "sdk/libraries/testing/src/TestFailure.pipe"},
        {"StaticAssertConsumer", entry, "StaticAssertConsumer.pipe"}};
    require(compileProjectModules(testing, "StaticAssertConsumer", "static_assert_consumer").at("ok") == true,
        "SDK static Assert or instance Assertions compatibility was rejected");
}

void qualifiedStaticApplicationsOnLargeProject() {
    const std::string helpers = R"pipe(module Helpers;
public Class Echoes {
    public static pure T Copy<T>(T value) => value;
}
)pipe";
    const std::string entry = R"pipe(module Entry;
import Helpers;
import Payload;
public model Class App {
    public pure int Number() => Echoes.Copy<int>(42);
    public pure bool Flag() => Helpers.Echoes.Copy<bool>(true);
    public pure string Text() => Echoes.Copy<string>("ok");
}
)pipe";

    std::vector<ModuleSource> modules{{"Helpers", helpers, "helpers/Echoes.pipe"},
        {"Entry", entry, "app/App.pipe"}};
    for (int group = 0; group < 12; ++group) {
        std::string source = "module Payload; public Class Payload" + std::to_string(group) + " { ";
        for (int method = 0; method < 5; ++method) {
            source += "public pure int Work" + std::to_string(method) + "(int value) { int total = 0; ";
            for (int statement = 0; statement < 60; ++statement)
                source += "total = total + value; ";
            source += "return total; } ";
        }
        source += '}';
        modules.push_back({"Payload", std::move(source), "payload/Part" + std::to_string(group) + ".pipe"});
    }

    require(compileProjectModules(modules, "Entry", "preplanned_static_members").at("ok") == true,
        "resolved static applications charged repeated whole-project trial clones");

    const std::string shadow = R"pipe(module Shadow;
import Helpers;
public Class Local { public pure T Copy<T>(T value) => value; }
public model Class App {
    public pure int Read(Local Echoes) => Echoes.Copy<int>(42) + Helpers.Echoes.Copy<int>(1);
}
)pipe";
    require(compileProjectModules({{"Helpers", helpers, "helpers/Echoes.pipe"},
        {"Shadow", shadow, "app/Shadow.pipe"}}, "Shadow", "preplanned_static_shadow").at("ok") == true,
        "static planning used a type in place of its shadowing instance receiver");
}

void crossModuleDiagnostics() {
    const std::string library = R"pipe(module Library;
public Class Assertions {
    private pure T Hidden<T>(T value) => value;
    public pure bool Equal<T>(T expected, T actual) => expected == actual;
}
)pipe";
    const std::string entry = R"pipe(module Entry;
import Library;
public Class App {
    public pure int Read(Assertions assertions) => assertions.Hidden<int>(1);
}
)pipe";
    try {
        compileModuleObjects({{"Library", library, "library/Assertions.pipe"},
            {"Entry", entry, "app/App.pipe"}}, "Entry", {"Entry.App"}, "member_diagnostic");
    } catch (const Diagnostic& diagnostic) {
        require(std::string(diagnostic.what()).find("private object method") != std::string::npos,
            "cross-module generic access diagnostic was weakened");
        require(diagnostic.module == "Entry" && diagnostic.path == "app/App.pipe",
            "cross-module generic access diagnostic lost caller provenance");
        require(diagnostic.offset == entry.find("Hidden<int>"), "generic access diagnostic offset changed");
        return;
    }
    throw std::runtime_error("private cross-module generic method admitted");
}

void fixtures(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto library = read(root / "tests/native-models/GenericMemberCallsLibrary.pipe");
    const auto entry = read(root / "tests/native-models/GenericMemberCalls.pipe");
    const std::vector<ModuleSource> modules{
        {"GenericMemberLibrary", library, "GenericMemberCallsLibrary.pipe"},
        {"GenericMemberApp", entry, "GenericMemberCalls.pipe"}};
    const auto response = compileModuleObjects(modules, "GenericMemberApp", {"GenericMemberApp.App"},
        "generic_member_calls");
    require(response.at("ok") == true, "cross-module member runtime fixture failed");
    write(output / "GenericMemberCalls.hpp", response.at("header").get<std::string>());
    write(output / "GenericMemberCalls.expected.json", response.dump());
    write(output / "GenericMemberCalls.request.json", Json{{"protocol", "pipelang.native-model.v3"},
        {"modules", Json::array({{{"name", modules[0].name}, {"source", library}, {"path", modules[0].path}},
            {{"name", modules[1].name}, {"source", entry}, {"path", modules[1].path}}})},
        {"entry", "GenericMemberApp"}, {"models", {"GenericMemberApp.App"}},
        {"namespace", "generic_member_calls"}}.dump());
    const auto project = compileProjectModules(modules, "GenericMemberApp", "generic_member_project");
    require(project.at("ok") == true, "project member receiver typing failed");
    write(output / "GenericMemberCallsProject.hpp", project.at("header").get<std::string>());
    write(output / "Types.hpp", project.at("types_header").get<std::string>());
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: generic-member-calls-tests SOURCE_ROOT OUTPUT_ROOT");
        contracts();
        staticContracts();
        qualifiedStaticApplicationsOnLargeProject();
        crossModuleDiagnostics();
        require(std::filesystem::create_directory(argv[2]), "generic member output must be a fresh directory");
        fixtures(argv[1], argv[2]);
        staticFixtures(argv[1], argv[2]);
        std::cout << assertions << " generic instance member assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
