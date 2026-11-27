// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/legacy-compiler/legacy.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include "windows_junction.hpp"

namespace {
using namespace pipelang::legacy;
template<class Action>
void refuses(Action action, const std::string& name) {
    bool refused = false;
    try {
        action();
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "expected refusal: " + name);
}
void semantics() {
    const std::vector<std::string> invalid{
        "Class X { int A = true; }",
        "Class X { int A; int A; }",
        "Class X {} Class X {}",
        "Class X { Missing A; }",
        "Class X : Missing {}",
        "Interface I { int A; } Class X : I { private int A; }",
        "Interface I { int M(int a); } Class X : I { int M(bool a) => 1; }",
        "Class X { int M(int a, int a) => a; }",
        "Class X { int A = B; int B; }",
        "Class X { int M() => unknown; }",
        "Class X { bool M() => 1 == true; }",
        "Class X { int M() => 1 / 2; }",
        "Class X { int M() => 1 + true; }",
        "Class X { int M() { return 1; } }",
        "Class X { int M() => 9223372036854775808; }",
        "Class X { int M() => 0x10; }"};
    for (const auto& source : invalid)
        refuses([&] { check(parse(source, "refusal.pipe")); }, source);
    const auto arithmetic = parse("Class X { int Wrap(int a) => a + 1; float Divide() => 7 / 2; bool Compare() => 9007199254740992 < 9007199254740993; }", "arithmetic.pipe");
    require(invoke(arithmetic, "X", "Wrap", {"9223372036854775807"}).string() == "-9223372036854775808", "legacy int64 wrap differs");
    require(invoke(arithmetic, "X", "Divide", {}).string() == "3.5", "legacy division must produce float");
    require(invoke(arithmetic, "X", "Compare", {}).string() == "false", "legacy ordered comparisons must preserve float conversion");
    const auto eager = parse("Class X { bool M() => false && 1 / 0 > 0; }", "eager.pipe");
    refuses([&] { invoke(eager, "X", "M", {}); }, "legacy boolean evaluation is eager");
    const auto shadow = parse("Class X { private string Prefix = \"field\"; string M(string Prefix) => Prefix; private int Hidden() => 1; }", "shadow.pipe");
    require(invoke(shadow, "X", "M", {"parameter"}).string() == "parameter", "parameter must shadow field");
    refuses([&] { invoke(shadow, "X", "Hidden", {}); }, "private method invocation");
    refuses([&] { invoke(shadow, "X", "M", {}); }, "argument count");
    refuses([&] { argument("int", "9223372036854775808"); }, "argument overflow");
    refuses([&] { argument("bool", "yes"); }, "bool argument");
    refuses([&] { parse(std::string(65537, ' '), "large.pipe"); }, "source size bound");
    refuses([&] { parse("Class X { int M() => " + std::string(60, '(') + "1" + std::string(60, ')') + "; }", "deep.pipe"); }, "expression depth bound");
    require(argument("bool", "TRUE").string() == "true" && argument("int", "+12").string() == "12", "legacy argument spellings differ");
}
void cli(const fs::path& executable, const fs::path& output) {
    const auto module = output / "cli-module";
    const auto generated = output / "cli-artifacts";
    atomicWrite(module / "config.yml", "");
    const std::string source = "Class X { private string Prefix = \"a'\"; string Name = \"<&>\"; int Count = 7; string Join(string suffix) => Prefix + suffix; }";
    atomicWrite(module / "X.pipe", source);
    atomicWrite(module / ".pipelang" / "invalid.pipe", "ignored generated material");
    const auto run = [&](const std::string& operation, std::vector<std::string> options = {}, const std::string& input = "") {
        std::vector<std::string> arguments{executable.string(), operation, "--profile", "v0.0.0.1", "--in", (module / "X.pipe").string()};
        arguments.insert(arguments.end(), options.begin(), options.end());
        return process(arguments, input);
    };
    const auto checked = run("check", {"--format", "json"});
    require(checked.exit == 0 && checked.output == "{\n  \"schema\": 1,\n  \"diagnostics\": []\n}\n", "CLI check contract differs");
    const auto compiled = run("compile", {"--out", generated.string()});
    require(compiled.exit == 0 && compiled.output.empty(), "CLI compile failed: " + compiled.diagnostic);
    const auto reference = compile(parse(source, "X.pipe"), "X");
    require(readFile(generated / "X.workflow.yml", 65536) == reference.workflow &&
            readFile(generated / "X.bindings.json", 65536) == reference.bindings &&
            readFile(generated / "X.bindings.env", 65536) == reference.environment, "CLI artifact transport differs");
    const auto invoked = run("invoke", {"--method", "Join", "--arg", "literal $(touch nope);", "--format", "env"});
    require(invoked.exit == 0 && invoked.output == "PIPELANG_RESULT_TYPE=string\nPIPELANG_RESULT='a'\"'\"'literal $(touch nope);'\n", "CLI literal argument/env result differs");
    const auto json = run("invoke", {"--method", "Join", "--arg", "end", "--format", "json"});
    require(json.exit == 0 && parseJson(json.output) == Json({{"class", "X"}, {"method", "Join"}, {"type", "string"}, {"value", "a'end"}}), "CLI JSON result differs");
    require(run("check", {"--stdin"}, "Class X { int A = true; }").exit != 0, "CLI ignored stdin replacement");
    require(readFile(module / "X.pipe", 65536) == source, "CLI changed source during stdin check");
    require(run("invoke", {"--method", "Join", "--format", "yaml"}).exit != 0, "CLI accepted invalid output format");
    require(process({executable.string(), "check", "--in", (module / "X.pipe").string()}).exit == 0, "CLI broke the active editor check contract");
    require(run("check", {"--profile", "v0.115.0"}).exit != 0, "CLI silently accepted an unported profile");
    const auto before = fileHash(generated / "X.workflow.yml");
    fs::remove(generated / "X.bindings.env");
    fs::create_directory(generated / "X.bindings.env");
    require(run("compile", {"--out", generated.string()}).exit != 0, "CLI replaced an unowned output directory");
    require(fileHash(generated / "X.workflow.yml") == before, "failed CLI publication changed existing artifact");
#ifdef _WIN32
    const auto junctionTarget = output / "cli-junction-target";
    atomicWrite(junctionTarget / "protected", "outside CLI sources");
    pipelang::tests::DirectoryJunction junction(module / "alias.pipe", junctionTarget);
#else
    fs::create_symlink(module / "X.pipe", module / "alias.pipe");
#endif
    require(run("check").exit != 0, "CLI admitted symbolic sources");
#ifdef _WIN32
    junction.remove();
    require(readFile(junctionTarget / "protected", 128) == "outside CLI sources", "CLI fixture changed its junction target");
#endif
}
void frozen(const fs::path& source, const fs::path& output) {
    const auto inventory = readFile(source / "docs/agents/tasks/fixtures/pipelang-vnext/legacy-v0.0.0.1-inventory.txt", 65536);
    std::istringstream lines(inventory);
    std::string line;
    std::vector<std::string> paths;
    while (std::getline(lines, line)) {
        if (line.starts_with("frozen-v0.0.0.1 "))
            paths.push_back(line.substr(16));
    }
    require(paths.size() == 45, "frozen legacy inventory differs");
    std::string sources;
    std::string parsed;
    std::string artifacts;
    std::string evaluation;
    std::map<std::string, std::map<std::string, std::string>> groups;
    std::map<std::string, Program> programs;
    std::size_t interfaceFiles = 0;
    std::size_t classFiles = 0;
    std::size_t methods = 0;
    for (const auto& path : paths) {
        const auto bytes = readFile(source / "tests/pipelangcompat/fixtures/legacy-v0.0.0.1" / path, 65536);
        sources += path + '\0' + bytes + '\0';
        auto program = parse(bytes, path);
        parsed += projection(program, path);
        interfaceFiles += std::any_of(program.declarations.begin(), program.declarations.end(), [](const Declaration& owner) { return owner.contract; });
        classFiles += std::any_of(program.declarations.begin(), program.declarations.end(), [](const Declaration& owner) { return !owner.contract; });
        programs.emplace(path, std::move(program));
        groups[fs::path(path).parent_path().generic_string()][path] = bytes;
    }
    for (const auto& [directory, files] : groups) {
        const auto program = merge(files);
        std::vector<std::string> owners;
        for (const auto& owner : program.declarations) {
            if (!owner.contract && owner.visible)
                owners.push_back(owner.name);
        }
        std::sort(owners.begin(), owners.end());
        for (const auto& owner : owners) {
            const auto compiled = compile(program, owner);
            artifacts += directory + '\0' + owner + '\0' + compiled.workflow + '\0' + compiled.bindings + '\0' + compiled.environment + '\0';
            const auto path = output / "artifacts" / directory;
            atomicWrite(path / (owner + ".workflow.yml"), compiled.workflow);
            atomicWrite(path / (owner + ".bindings.json"), compiled.bindings);
            atomicWrite(path / (owner + ".bindings.env"), compiled.environment);
        }
        for (const auto& [path, bytes] : files) {
            for (const auto& owner : programs.at(path).declarations) {
                if (owner.contract || !owner.visible)
                    continue;
                for (const auto& method : owner.methods) {
                    if (!method.visible)
                        continue;
                    std::vector<std::string> arguments;
                    for (const auto& parameter : method.parameters) {
                        if (parameter.type == "bool")
                            arguments.push_back("false");
                        else if (parameter.type == "string")
                            arguments.emplace_back();
                        else
                            arguments.push_back("0");
                    }
                    const auto value = invoke(program, owner.name, method.name, arguments);
                    evaluation += directory + '\0' + owner.name + '\0' + method.name + '\0' + method.result + '\0' + value.string() + '\0';
                    ++methods;
                }
            }
        }
    }
    const Json digests{{"source", sha256(sources)}, {"projection", sha256(parsed)}, {"artifacts", sha256(artifacts)}, {"evaluation", sha256(evaluation)}};
    atomicWrite(output / "digests.json", canonical(digests));
    require(interfaceFiles == 22 && classFiles == 31 && methods == 1, "frozen declaration/method inventory differs");
    require(digests.at("source") == "b0d44c2af52021c1616ae4ffd4bbc234433b36a327cfdbf2403b9ec880d8857c", "frozen source digest differs");
    require(digests.at("projection") == "a68f1924df93447e42265e4510a53dd96c7bb1e05a58fc1b8db9d322f97b3815", "frozen parsed projection differs");
    require(digests.at("artifacts") == "167fc9825c3e56b7541a80b1d309e4ecd93eb37f45bdb00630838077f9a48597", "frozen artifact bytes differ");
    require(digests.at("evaluation") == "c10ff75686f372bb81dfb9cdca0aaaee708234a56a0bf6ef9d783a7ed672a3a0", "frozen evaluation differs");
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 4, "legacy compiler tests require source root, fresh output and installed CLI");
        const fs::path output = argv[2];
        require(!fs::exists(output), "legacy proof output must be fresh");
        fs::create_directories(output);
        frozen(argv[1], output);
        semantics();
        cli(argv[3], output);
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"profile", "v0.0.0.1"},
            {"files", 45}, {"interface_files", 22}, {"class_files", 31}, {"methods", 1}, {"frozen_digests", 4},
            {"semantic_refusals", 23}, {"cli", "installed-process-tested"}}));
        std::cout << "C++ legacy v0.0.0.1: frozen source/projection/artifact/evaluation digests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
