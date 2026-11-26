// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../../src/native/model-compiler/compiler.hpp"
#include "../../../src/native/model-compiler/composition.hpp"
#include "../../../src/native/model-compiler/CompositionFactoryProfile.hpp"
#include "../../../src/native/model-compiler/CompositionPlanner.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

void refused(const std::string& source, const std::string& expected) {
    try {
        (void)compileObjects(source, {"App"}, "empty_refusal");
    } catch (const Diagnostic& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
            "expected " + expected + ", got " + error.what());
        return;
    }
    throw std::runtime_error("invalid empty-interface call admitted: " + expected);
}

void negativeContracts() {
    refused(R"pipe(
public source Interface IEmpty { int Read(); }
public Class App { public pure int Read(IEmpty item) => item.Read(); }
)pipe", "pure methods cannot call object effects");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(int value); }
public Class App { public pure int Read(IEmpty item) => item.Read(true); }
)pipe", "no exact matching method overload");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class App { public pure bool Read(IEmpty item) => item.Read(); }
)pipe", "expected bool, got int");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class App { public pure int Read(IEmpty item) => item.Unknown(); }
)pipe", "no exact matching method overload");
    refused("public Class App { public pure int Read(Unknown item) => item.Read(); }",
        "unknown value or service type");
    refused(R"pipe(
public source Interface IEmpty { Task<Result<int,string>> Read(); }
public Class App {
    public async Task<Result<int,string>> Run(IEmpty item) {
        Result<int,string> result = item.Read();
        return result;
    }
}
)pipe", "directly awaited");
    refused(R"pipe(
public source Interface IEmpty { Task<Result<int,string>> Read(); }
public Class App {
    private Lock gate;
    public async Task<Result<int,string>> Run(IEmpty item) {
        lock (gate) { Result<int,string> result = await item.Read(); }
        return Result<int,string>.Success(1);
    }
}
)pipe", "lock bodies cannot suspend");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class App {
    public void Run(IEmpty item, List<int> values) {
        parallel foreach (int value in values) { int result = item.Read(); }
    }
}
)pipe", "owner-confined object");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class App {
    public void Run(List<IEmpty> values) {
        parallel foreach (IEmpty item in values) { int result = item.Read(); }
    }
}
)pipe", "parallel inputs cannot contain owner-confined object references");
    refused(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class Wrong : IEmpty { public int Read() => 1; }
public Class App {}
)pipe", "exact source interface contract");
    refused(R"pipe(
public source Interface IEmpty { Task<Result<int,string>> Read(); }
public Class Wrong : IEmpty { public Result<int,string> Read() => Result<int,string>.Success(1); }
public Class App {}
)pipe", "exact source interface contract");
}

void checkedEmptyEdges() {
    auto program = parse(R"pipe(
public source Interface IEmpty { pure int Read(); }
public Class Caller { public pure int Read(IEmpty item) => item.Read(); }
)pipe", true);
    program.objectEnabled = true;
    check(program);
    const auto& contract = program.declarations.at(0).methods.at(0);
    const auto& caller = program.declarations.at(1).methods.at(0);
    const auto& call = caller.body.at(0).expression;
    require(call->resolution == "source_interface_call" && call->callable == contract.identity,
        "known empty dispatch lost its selected declared contract");
    require(call->targets.empty() && caller.calls.empty() && contract.dispatchTargets.empty(),
        "abstract declaration was invented as an executable implementation");

    const auto implemented = compileObjects(R"pipe(
public source Interface IValue { pure int Read(); }
public Class Value : IValue { public pure int Read() => 31; }
public Class App { public int Read() { IValue value = new Value(); return value.Read(); } }
)pipe", {"App"}, "inhabited_interface");
    require(implemented.at("ok") == true, "later closed compilation lost its real implementation");
    require(implemented.at("manifest").at("objects").at(0).at("implements").size() == 1,
        "nonempty closed implementation relationship was lost");
}

const Json& named(const Json& result, const std::string& category, const std::string& sourceName) {
    for (const auto& item : result.at("manifest").at(category)) {
        if (item.value("sourceName", item.at("name").get<std::string>()) == sourceName)
            return item;
    }
    throw std::runtime_error("missing manifest declaration: " + sourceName);
}

std::string dispatcher(const std::string& generated, const std::string& owner, bool asynchronous) {
    const auto receiver = "pipelang::model::ObjectRef<pl_object_tag_" + owner + "> pl_self";
    const auto returned = asynchronous ? "pipelang::model::Coroutine<" : "std::int64_t ";
    std::istringstream lines(generated);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(receiver) == std::string::npos || line.find(returned) == std::string::npos)
            continue;
        const auto start = line.find("pl_object_function_");
        if (start == std::string::npos)
            continue;
        const auto opening = line.find('(', start);
        require(opening != std::string::npos, "dispatcher declaration lacks parameters");
        return line.substr(start, opening - start);
    }
    throw std::runtime_error("missing empty-interface dispatcher declaration");
}

void emit(const std::filesystem::path& directory, const Json& result, bool project, bool compiled) {
    std::filesystem::create_directories(directory);
    const auto header = result.at("header").get<std::string>();
    write(directory / "Model.hpp", header);
    write(directory / "manifest.json", result.at("manifest").dump(2));
    std::string generated = header;
    if (result.contains("types_header"))
        write(directory / "Types.hpp", result.at("types_header").get<std::string>());
    if (result.contains("implementation")) {
        for (const auto& file : result.at("implementation").at("files")) {
            const auto name = file.at("name").get<std::string>();
            require(std::filesystem::path(name).filename() == name, "unsafe generated artifact name");
            const auto content = file.at("content").get<std::string>();
            write(directory / name, content);
            generated += content;
        }
    }

    const auto qualified = [&](const std::string& module, const std::string& type) {
        return project ? module + '.' + type : type;
    };
    const auto app = named(result, "classes", qualified("Consumer", "App")).at("name").get<std::string>();
    const auto unused = named(result, "sourceInterfaces", qualified("TestingBase", "IUnused")).at("name").get<std::string>();
    const auto child = named(result, "sourceInterfaces", qualified("TestingBase", "IEmptyChild")).at("name").get<std::string>();
    const auto testCase = named(result, "sourceInterfaces", qualified("TestingBase", "ITestCase")).at("name").get<std::string>();
    const auto live = named(result, "objects", qualified("TestingBase", "LiveBase")).at("name").get<std::string>();
    const auto readFunction = dispatcher(generated, unused, false);
    const auto laterFunction = dispatcher(generated, unused, true);

    std::string boundary = "// Generated fixture bridge; selects actual emitted dispatch symbols.\n#pragma once\n#include \"Model.hpp\"\n";
    if (compiled)
        boundary += "#include \"Objects.hpp\"\n";
    boundary += "#include <array>\n#include <cstdint>\n#include <utility>\nnamespace empty_interface_fixture {\n";
    boundary += "using App = ::empty_interfaces::" + app + ";\n";
    for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
            {"UnusedTag", unused}, {"ChildTag", child}, {"TestCaseTag", testCase}, {"LiveTag", live}})
        boundary += "using " + entry.first + " = ::empty_interfaces::pl_object_tag_" + entry.second + ";\n";
    boundary += "using Unused = pipelang::model::ObjectRef<UnusedTag>;\n";
    boundary += "using Child = pipelang::model::ObjectRef<ChildTag>;\n";
    boundary += "using TestCase = pipelang::model::ObjectRef<TestCaseTag>;\n";
    boundary += "inline std::int64_t directRead(pipelang::model::ObjectExecution& execution, Unused value) {\n";
    boundary += "    return ::empty_interfaces::" + readFunction + "(execution, std::move(value));\n}\n";
    boundary += "inline pipelang::model::Coroutine<pipelang::model::Result<std::int64_t>> directLater(pipelang::model::ObjectExecution& execution, Unused value) {\n";
    boundary += "    return ::empty_interfaces::" + laterFunction + "(execution, std::move(value), {});\n}\n";
    const auto& objects = result.at("manifest").at("objects");
    boundary += "inline constexpr std::array<std::uint64_t, " + std::to_string(objects.size()) + "> concreteIds{\n";
    for (const auto& object : objects)
        boundary += "    ::empty_interfaces::pl_object_tag_" + object.at("name").get<std::string>() + "::typeId,\n";
    boundary += "};\n}\n";
    write(directory / "Boundary.hpp", boundary);
}

std::string unmodular(const std::string& source) {
    std::istringstream lines(source);
    std::string result;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("module ") && !line.starts_with("import "))
            result += line + '\n';
    }
    return result;
}

void factoryClosure(const std::filesystem::path& sourceRoot, const std::filesystem::path& output) {
    const auto source = read(sourceRoot / "Factory.pipe");
    const std::vector<ModuleSource> modules{{"EmptyFactory", source, "Factory.pipe"}};
    const auto response = compileProjectCompositionFactory(modules, "EmptyFactory", "empty_factory");
    require(response.at("ok") == true, "finite empty factory/constructor closure was refused");
    std::filesystem::create_directories(output / "factory");
    write(output / "factory/response.json", response.dump(2));

    auto program = linkModules(modules, "EmptyFactory", true, true);
    program.projectEnabled = true;
    program.objectEnabled = true;
    normalizeAndCheckComposition(program);
    const auto graphs = planCompositions(program);
    std::size_t poisonedContracts = 0;
    for (auto& declaration : program.declarations) {
        if (!declaration.sourceInterface)
            continue;
        for (auto& method : declaration.methods) {
            // Synthetic post-check poison: abstract declarations must never be
            // traversed as executable bodies by either factory closure walker.
            Statement forbidden;
            forbidden.kind = "parallel_foreach";
            method.body.push_back(std::move(forbidden));
            ++poisonedContracts;
        }
    }
    require(poisonedContracts == 1, "factory closure witness lost its empty contract");
    CompositionFactoryProfile(program, graphs).admit();
    require(true, "abstract declaration was treated as an executable factory helper");

    std::vector<Expr> calls;
    for (auto& declaration : program.declarations) {
        for (auto& method : declaration.methods) {
            std::vector<Statement*> pending;
            for (auto& statement : method.body)
                pending.push_back(&statement);
            while (!pending.empty()) {
                auto* statement = pending.back();
                pending.pop_back();
                std::vector<Expr> expressions{statement->expression, statement->target};
                while (!expressions.empty()) {
                    const auto expression = expressions.back();
                    expressions.pop_back();
                    if (!expression)
                        continue;
                    if (expression->resolution == "source_interface_call")
                        calls.push_back(expression);
                    expressions.insert(expressions.end(), expression->children.begin(), expression->children.end());
                }
                for (auto* branch : {&statement->initialization, &statement->body, &statement->iteration,
                                     &statement->alternative, &statement->cleanup}) {
                    for (auto& nested : *branch)
                        pending.push_back(&nested);
                }
            }
        }
    }
    require(calls.size() == 2, "factory/constructor lost their empty call sites");
    for (const auto& call : calls) {
        const auto declared = call->callable;
        call->callable = "missing.checked.contract()";
        bool rejected = false;
        try {
            CompositionFactoryProfile(program, graphs).admit();
        } catch (const Diagnostic& error) {
            require(std::string(error.what()).find("unresolved checked source interface contract") != std::string::npos,
                "missing contract was confused with a closed empty set");
            rejected = true;
        }
        call->callable = declared;
        require(rejected, "unresolved source interface contract was silently treated as EMPTY");
    }
}

void fixtures(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto fixtureRoot = root / "tests/native-models/empty-source-interfaces";
    const auto shared = read(fixtureRoot / "TestingBase.pipe");
    const auto asynchronous = read(fixtureRoot / "AsyncTesting.pipe");
    auto consumer = read(fixtureRoot / "Consumer.pipe");
    const auto ordinary = unmodular(shared) + unmodular(asynchronous) + unmodular(consumer);
    emit(output / "objects", compileObjects(ordinary, {"App"}, "empty_interfaces"), false, false);

    const auto declaration = consumer.find("public Class App {");
    require(declaration != std::string::npos, "async-only consumer has no root");
    consumer.replace(declaration, std::string("public Class App {").size(), "public model Class App {");
    const std::vector<ModuleSource> modules{
        {"TestingBase", shared, "TestingBase.pipe"},
        {"AsyncTesting", asynchronous, "AsyncTesting.pipe"},
        {"Consumer", consumer, "Consumer.pipe"}};
    const auto project = compileProjectModules(modules, "Consumer", "empty_interfaces");
    const auto compiled = compileProjectImplementation(modules, "Consumer", "empty_interfaces", "sources");
    require(project.at("manifest") == compiled.at("manifest"), "compiled empty-interface manifest diverged");
    require(named(project, "sourceInterfaces", "TestingBase.IUnused").at("methods").size() == 2,
        "declaration-only unused interface was discarded");
    require(named(project, "sourceInterfaces", "TestingBase.IDeclaredOnly").at("methods").size() == 1,
        "unreferenced declaration-only interface was discarded");
    require(named(project, "sourceInterfaces", "TestingBase.ITestCase").at("methods").size() == 1,
        "imported synchronous contract was discarded");
    for (const auto& object : project.at("manifest").at("objects")) {
        for (const auto& implemented : object.at("implements")) {
            require(implemented != named(project, "sourceInterfaces", "TestingBase.ITestCase").at("name"),
                "async-only consumer unexpectedly acquired a synchronous implementer");
        }
    }
    emit(output / "project", project, true, false);
    emit(output / "compiled", compiled, true, true);
    factoryClosure(fixtureRoot, output);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: empty-source-interface-compiler-tests SOURCE_ROOT FRESH_OUTPUT");
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path output(argv[2]);
        require(root.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
            "absolute source root and fresh output required");
        negativeContracts();
        checkedEmptyEdges();
        fixtures(root, output);
        std::cout << "empty source interfaces: " << assertions << " compiler assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
