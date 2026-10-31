// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/legacy-compiler/legacy.hpp"
#include <iostream>

namespace {
using namespace pipelang::legacy;
void test(const fs::path& output, const fs::path& executable) {
    const auto module = output / "module";
    atomicWrite(module / "config.yml", "");
    const auto source = module / "buffer.pipe";
    atomicWrite(source, "Class Buffer { int Value = 1; }");
    const auto before = fileHash(source);
    const auto check = [&](const std::string& bytes, const std::string& format) {
        return process({executable.string(), "check", "--profile", "v0.0.0.1", "--in", source.string(),
            "--stdin", "--format", format}, bytes);
    };
    const auto valid = check("Class Buffer { int Value = 2; }", "json");
    require(valid.exit == 0 && valid.output == "{\n  \"schema\": 1,\n  \"diagnostics\": []\n}\n", "valid check JSON changed");
    const std::string broken = "Class Broken { int Value = ; }";
    const auto failed = check(broken, "json");
    require(failed.exit != 0, "broken check succeeded");
    const auto diagnostic = parseJson(failed.output).at("diagnostics").at(0);
    require(diagnostic.at("code") == "PL2001" && diagnostic.at("category") == "syntax" && diagnostic.at("severity") == "error", "syntax diagnostic contract changed");
    const auto offset = broken.find(';');
    require(diagnostic.at("primary").at("file") == source.generic_string() && diagnostic.at("primary").at("start") ==
        Json({{"offset", offset}, {"line", 1}, {"column", offset + 1}, {"utf16_column", offset + 1}}), "syntax diagnostic position changed");
    const auto rendered = check(broken, "text");
    require(rendered.exit != 0 && rendered.output.starts_with(source.string() + ":1:" + std::to_string(offset + 1) + ": error[PL2001]: "), "text diagnostic position changed");
    const std::string unicode = "// editor buffer\nClass Broken { string Value = \"\xf0\x9f\x99\x82\" + ; }";
    const auto unicodeResult = check(unicode, "json");
    const auto primary = parseJson(unicodeResult.output).at("diagnostics").at(0).at("primary");
    const auto unicodeOffset = unicode.find(';');
    const auto byteColumn = unicodeOffset - unicode.find('\n');
    require(primary.at("start") == Json({{"offset", unicodeOffset}, {"line", 2}, {"column", byteColumn - 3}, {"utf16_column", byteColumn - 2}}), "Unicode source/editor columns differ");
    const auto semantic = parseJson(check("Class X { int A = true; }", "json").output).at("diagnostics").at(0);
    require(semantic.at("code") == "PL3009" && semantic.at("category") == "semantic" &&
        semantic.at("message") == "class X field A default type bool does not match int", "semantic diagnostic code/message differs");
    require(semantic.at("related").at(0).at("message") == "declared field type" &&
        semantic.at("related").at(0).at("range").at("start").at("offset") == 10 &&
        semantic.at("related").at(0).at("range").at("end").at("offset") == 13, "field diagnostic lost declared type span");
    const std::vector<std::pair<std::string, std::string>> semanticCases{
        {"Class X {} Class X {}", "PL3003"},
        {"Class X { int A; int A; }", "PL3005"},
        {"Class X { Missing A; }", "PL3006"},
        {"Class X : Missing {}", "PL3007"},
        {"Interface I { int A; } Class X : I {}", "PL3008"},
        {"Interface I { private int A; } Class X {}", "PL3004"},
        {"Class X { int M(int a, int a) => a; }", "PL3005"},
        {"Class X { int M() => true; }", "PL3009"}};
    for (const auto& [bytes, code] : semanticCases) {
        const auto value = parseJson(check(bytes, "json").output).at("diagnostics").at(0);
        require(value.at("code") == code && value.at("category") == "semantic" && !value.at("message").get<std::string>().empty(), "semantic diagnostic class differs: " + bytes);
        if (code != "PL3004")
            require(value.contains("related") && !value.at("related").empty(), "semantic diagnostic lost its related location: " + bytes);
    }
    const auto unknown = parseJson(check("Class X { int M() => missing; }", "json").output).at("diagnostics").at(0);
    require(unknown.at("message") == "class X method M: unknown identifier \"missing\"", "expression diagnostic context differs");
    const std::string tiny = "public Class Root { public bool Ready(int count) => count > 0; }";
    const auto parsed = parse(tiny, "root.pipe");
    const auto& owner = parsed.declarations.at(0);
    const auto& method = owner.methods.at(0);
    require(owner.span.start == 0 && owner.span.end == 64 && method.resultSpan.start == 27 && method.resultSpan.end == 31 &&
        method.parameters.at(0).typeSpan.start == 38 && method.parameters.at(0).typeSpan.end == 41 &&
        method.parameters.at(0).span.start == 38 && method.parameters.at(0).span.end == 47 &&
        method.body->start == 52 && method.body->end == 61, "frontend spans differ from the frozen tiny HIR locations");
    require(compile(parse("Class int {}", "shadow.pipe"), "int").entry == "int", "legacy declaration namespace was incorrectly restricted");
    const auto onlyInterface = parseJson(check("Interface I { int A; }", "json").output).at("diagnostics").at(0);
    require(onlyInterface.at("code") == "PL3010", "interface-only check lost entry diagnostic");
    const std::string invalid = "Class X {}\n" + std::string(1, static_cast<char>(0xff));
    const auto invalidJson = parseJson(check(invalid, "json").output).at("diagnostics").at(0);
    require(invalidJson.at("code") == "PL0001" && invalidJson.at("category") == "source", "invalid UTF-8 source diagnostic differs");
    const auto invalidOffset = invalid.size() - 1;
    require(invalidJson.at("primary").at("start") == Json({{"offset", invalidOffset}, {"line", 1}, {"column", invalidOffset + 1}, {"utf16_column", invalidOffset + 1}}), "unadmitted UTF-8 file position differs");
    atomicWrite(module / "a.pipe", "Class A { int Value = ; }");
    atomicWrite(module / "z.pipe", "Class Z { int Value = ; }");
    const auto multiple = check("Class Buffer {}", "json");
    const auto ordered = parseJson(multiple.output).at("diagnostics");
    require(ordered.size() == 2 && ordered[0].at("primary").at("file") == (module / "a.pipe").generic_string() &&
        ordered[1].at("primary").at("file") == (module / "z.pipe").generic_string(), "multi-file syntax diagnostic order differs");
    require(fileHash(source) == before, "check changed saved source bytes");
    require(!fs::exists(module / ".pipelang") && !fs::exists(module / "bin"), "check emitted artifacts");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"contracts", {"valid-json", "syntax-json", "text-location", "utf8-utf16-columns",
        "semantic-diagnostic", "entry-selection", "invalid-utf8", "multi-file-order", "unsaved-buffer", "inert-check",
        "semantic-code-related-span-matrix", "expression-context", "tiny-hir-source-spans", "legacy-declaration-namespace"}}}));
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 3, "diagnostic tests require fresh proof and installed CLI");
        const fs::path output = argv[1];
        require(output.is_absolute() && !fs::exists(output), "diagnostic proof must be fresh and absolute");
        fs::create_directories(output);
        test(output, argv[2]);
        std::cout << "14 C++ diagnostic contract groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "diagnostic oracle: " << error.what() << '\n';
        return 1;
    }
}
