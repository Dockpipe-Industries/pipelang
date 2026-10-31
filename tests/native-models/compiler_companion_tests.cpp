// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/sdk-tools/manifest.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace pipelang::tools;
Json cases = Json::array();
std::string repeated(const std::string& value, std::size_t count) {
    std::string result;
    result.reserve(value.size() * count);
    for (std::size_t index = 0; index < count; ++index)
        result += value;
    return result;
}
void record(const std::string& name, const std::string& input) {
    cases.push_back({{"id", name}, {"input_sha256", sha256(input)}, {"status", "passed"}});
}
ProcessResult invoke(const fs::path& executable, const std::string& bytes) {
    return process({executable.string()}, bytes, std::chrono::seconds(10), 32U << 20);
}
void refused(const fs::path& executable, const std::string& name, const std::string& bytes, const std::string& message = "") {
    const auto result = invoke(executable, bytes);
    require(result.exit == 2 && result.output.empty() && !result.diagnostic.empty(), "expected empty unsuccessful protocol refusal: " + name);
    require(result.diagnostic.find(message) != std::string::npos, "protocol refusal message differs: " + name);
    record(name, bytes);
}
Json response(const fs::path& executable, const std::string& bytes) {
    const auto result = invoke(executable, bytes);
    require(result.exit == 0 && result.diagnostic.empty(), "expected successful protocol response: " + result.diagnostic);
    return parseJson(result.output, 32U << 20);
}
Json lexerResponse(const fs::path& lexer, const std::string& source) {
    return response(lexer, Json({{"contract", "pipelang.lexer.request.v1"}, {"source", source}}).dump());
}
void lexer(const fs::path& executable) {
    const auto start = cases.size();
    const auto request = [](const Json& source) { return Json({{"contract", "pipelang.lexer.request.v1"}, {"source", source}}).dump(); };
    const auto expected = [](std::size_t offset) {
        return Json({{"contract", "pipelang.lexer.response.v1"}, {"tokens", Json::array({{{"kind", "eof"}, {"literal", ""}, {"start", offset}, {"end", offset}}})}, {"failure", nullptr}});
    };
    require(lexerResponse(executable, "") == expected(0), "empty lexer EOF differs");
    record("lexer/empty-eof", "");
    struct Failure {
        std::string name;
        std::string source;
        std::string code;
        std::string message;
        unsigned start;
        unsigned end;
    };
    const std::vector<Failure> failures{
        {"number", "12.", "PL1002", "invalid float literal", 0, 3},
        {"escape", "\"\\q\"", "PL1003", "invalid escape sequence", 1, 3},
        {"unterminated", "\"tail", "PL1004", "unterminated string literal", 0, 5},
        {"nul", std::string(1, '\0'), "PL1001", "unexpected character '\\x00'", 0, 1}};
    for (const auto& item : failures) {
        require(lexerResponse(executable, item.source) == Json({{"contract", "pipelang.lexer.response.v1"}, {"tokens", nullptr},
            {"failure", {{"code", item.code}, {"message", item.message}, {"start", item.start}, {"end", item.end}}}}), "lexical failure changed: " + item.name);
        record("lexer/" + item.name, item.source);
    }
    const auto valid = request("");
    unsigned index = 0;
    for (const auto& bytes : std::vector<std::string>{"", "{", "null", "[]", valid + " {}", valid + " garbage"})
        refused(executable, "lexer/framing-" + std::to_string(index++), bytes);
    index = 0;
    for (const auto& suffix : {R"(,"source":"other"})", R"(,"sour\u0063e":"other"})", R"(,"contract":"pipelang.lexer.request.v1"})"})
        refused(executable, "lexer/duplicate-" + std::to_string(index++), valid.substr(0, valid.size() - 1) + suffix);
    const std::vector<Json> missing{Json::object(), {{"contract", "pipelang.lexer.request.v1"}}, {{"source", ""}},
        {{"contract", "pipelang.lexer.request.v1"}, {"source", ""}, {"extra", nullptr}}};
    index = 0;
    for (const auto& value : missing)
        refused(executable, "lexer/fields-" + std::to_string(index++), value.dump());
    index = 0;
    for (const auto& value : std::vector<Json>{nullptr, false, 123, Json::array(), Json::object()})
        refused(executable, "lexer/source-type-" + std::to_string(index++), request(value));
    index = 0;
    for (const auto& value : std::vector<Json>{nullptr, false, 123, Json::array(), Json::object(), "", "pipelang.lexer.request.future"})
        refused(executable, "lexer/contract-type-" + std::to_string(index++), Json({{"contract", value}, {"source", ""}}).dump());
    refused(executable, "lexer/json-utf8", std::string(R"({"contract":"pipelang.lexer.request.v1","source":")") + static_cast<char>(0xff) + "\"}");
    refused(executable, "lexer/json-surrogate", R"({"contract":"pipelang.lexer.request.v1","source":"\ud800"})");
    refused(executable, "lexer/input-capacity", std::string((512U << 10) + 1, ' '));
    refused(executable, "lexer/source-capacity", request(std::string(65537, ' ')));
    refused(executable, "lexer/source-byte-capacity", request(repeated("\xc3\xa9", 32769)));
    require(lexerResponse(executable, std::string(65536, ' ')) == expected(65536), "maximum lexer source was refused");
    record("lexer/maximum-source", std::string(65536, ' '));
    refused(executable, "lexer/depth-capacity", std::string(18, '[') + '0' + std::string(18, ']'), "nesting limit");
    refused(executable, "lexer/node-capacity", '[' + repeated("null,", 64) + "null]", "node limit");
    require(cases.size() - start == 38, "migrated raw lexer case inventory differs");
}
std::string replace(const std::string& source, const std::string& before, const std::string& after) {
    const auto offset = source.find(before);
    require(offset != std::string::npos, "protocol oracle replacement did not match");
    return source.substr(0, offset) + after + source.substr(offset + before.size());
}
void view(const fs::path& lexer, const fs::path& parser) {
    const std::string source = "view V { Label(); }";
    const auto tokens = lexerResponse(lexer, source).at("tokens");
    const Json request{{"contract", "pipelang.view-parser.request.v1"}, {"file", "v.pipe"}, {"source_bytes", source.size()}, {"tokens", tokens}};
    const auto result = response(parser, request.dump());
    const auto span = [](unsigned start, unsigned end) { return Json({{"file", "v.pipe"}, {"start", start}, {"end", end}}); };
    const Json expected{{"contract", "pipelang.view-parser.response.v1"}, {"failure", nullptr},
        {"declaration", {{"Name", "V"}, {"Span", span(0, 19)}, {"State", Json::array()}, {"Events", Json::array()}, {"Actions", Json::array()},
            {"Root", {{"Component", "Label"}, {"Properties", Json::array()}, {"Children", Json::array()}, {"Span", span(9, 17)}}}}}};
    require(result == expected, "view companion AST or spans differ");
    record("view/lexer-parser-ast", request.dump());
    const auto valid = request.dump();
    const std::vector<std::vector<std::string>> mutations{
        {"unknown", "\"file\":", "\"extra\":false,\"file\":"},
        {"duplicate", "\"file\":", "\"file\":\"x\",\"file\":"},
        {"alias", "\"tokens\":", "\"Tokens\":"},
        {"missing", "\"file\":\"v.pipe\",", ""},
        {"null-string", "\"file\":\"v.pipe\"", "\"file\":null"},
        {"null-tokens", "\"tokens\":[", "\"tokens\":null,\"extra\":["},
        {"null-number", "\"source_bytes\":19", "\"source_bytes\":null"},
        {"float", "\"start\":0", "\"start\":0.0"},
        {"exponent", "\"start\":0", "\"start\":0e0"},
        {"overflow", "\"start\":0", "\"start\":18446744073709551616"},
        {"negative", "\"start\":0", "\"start\":-1"},
        {"token-duplicate", "\"kind\":", "\"kind\":\"other\",\"kind\":"},
        {"token-alias", "\"kind\":", "\"Kind\":"},
        {"unknown-kind", "\"kind\":\"identifier\"", "\"kind\":\"other\""},
        {"early-eof", "\"kind\":\"identifier\"", "\"kind\":\"eof\""},
        {"missing-eof", "\"kind\":\"eof\"", "\"kind\":\"identifier\""},
        {"overlap", "\"start\":5", "\"start\":0"},
        {"surrogate", "\"v.pipe\"", "\"\\ud800\""},
        {"utf8", "v.pipe", std::string(1, static_cast<char>(0xff))}};
    for (const auto& mutation : mutations)
        refused(parser, "view/" + mutation[0], replace(valid, mutation[1], mutation[2]));
    refused(parser, "view/trailing", valid + "{}");
    refused(parser, "view/nesting", "{\"contract\":" + std::string(8, '[') + std::string(8, ']') + '}');
    refused(parser, "view/size", std::string((16U << 20) + 1, ' '));
}
void emitter(const fs::path& executable, const fs::path& root, const fs::path& output) {
    auto program = parseJson(readFile(root / "src/lib/pipelang/testdata/tiny-pure-function.core.json", 1U << 20));
    // A hand-selected comparison-only pilot fixture. This is not source-profile
    // admission and does not replace the authoritative Core validation port.
    program["language_contract"] = "v0.113.0";
    const auto request = [&](const Json& value) { return Json({{"contract", "pipelang.cpp-emitter.request.v1"}, {"program", value}}).dump(); };
    const auto result = response(executable, request(program));
    require(result.at("contract") == "pipelang.cpp-emitter.response.v1" && result.at("functions").size() == 1, "pilot emitter response contract differs");
    const auto& identity = program.at("functions")[0].at("identity");
    const auto key = identity.at("package_id").get<std::string>() + '\0' + identity.at("path").get<std::string>();
    const auto symbol = "fn_" + sha256(key).substr(0, 24);
    require(result.at("functions")[0] == Json({{"identity", identity}, {"name", symbol}}), "pilot emitter semantic binding hash differs");
    atomicWrite(output / "Emitted.hpp", result.at("source").get<std::string>());
    atomicWrite(output / "emitted_probe.cpp", "#include \"Emitted.hpp\"\nint main() {\n    return pipelang::" + symbol + "(1) && !pipelang::" + symbol + "(0) && !pipelang::" + symbol + "(-1) ? 0 : 1;\n}\n");
    record("emitter/comparison-pilot-and-binding", request(program));
    const auto valid = request(program);
    unsigned index = 0;
    for (const auto& bytes : std::vector<std::string>{"", "{", "null", "[]", valid + " {}", valid + " garbage"})
        refused(executable, "emitter/framing-" + std::to_string(index++), bytes);
    refused(executable, "emitter/duplicate-root", "{\"contract\":\"pipelang.cpp-emitter.request.v1\",\"contract\":\"pipelang.cpp-emitter.request.v1\",\"program\":" + program.dump() + '}');
    refused(executable, "emitter/duplicate-nested", "{\"contract\":\"pipelang.cpp-emitter.request.v1\",\"program\":" + program.dump() + ",\"extra\":{\"name\":1,\"na\\u006de\":2}}");
    for (const auto* member : {"language_contract", "compiler_contract"}) {
        auto invalid = program;
        invalid[member] = "unsupported";
        refused(executable, std::string("emitter/closed-") + member, request(invalid));
    }
    for (const auto* member : {"body", "return_type", "identity"}) {
        auto invalid = program;
        invalid["functions"][0].erase(member);
        refused(executable, std::string("emitter/missing-") + member, request(invalid));
    }
    auto invalid = program;
    invalid["functions"][0]["body"]["kind"] = "unsupported_expression";
    refused(executable, "emitter/closed-expression", request(invalid));
    invalid = program;
    invalid["functions"][0]["body"]["type"] = {{"kind", "unsupported_type"}};
    refused(executable, "emitter/closed-type", request(invalid));
    for (const int position : {-1, 1000000}) {
        invalid = program;
        const auto type = invalid["functions"][0]["body"]["type"];
        invalid["functions"][0]["body"] = {{"kind", "reference"}, {"type", type}, {"parameter", position}};
        refused(executable, "emitter/missing-reference-" + std::to_string(position), request(invalid));
    }
    for (const auto* member : {"package_id", "path"}) {
        for (const auto& value : std::vector<Json>{"", -1}) {
            invalid = program;
            invalid["functions"][0]["identity"][member] = value;
            refused(executable, std::string("emitter/identity-") + member + '-' + value.dump(), request(invalid));
        }
    }
    invalid = program;
    invalid["functions"].push_back(invalid["functions"][0]);
    refused(executable, "emitter/duplicate-function", request(invalid));
    for (int position : {-1, 1}) {
        invalid = program;
        invalid["functions"][0]["parameters"] = Json::array({{{"position", position}, {"name", "parameter"}, {"type", {{"kind", "primitive"}, {"primitive", "bool"}}}}});
        refused(executable, "emitter/parameter-position-" + std::to_string(position), request(invalid));
    }
    refused(executable, "emitter/missing-program", R"({"contract":"pipelang.cpp-emitter.request.v1"})");
    refused(executable, "emitter/future-contract", Json({{"contract", "pipelang.cpp-emitter.request.future"}, {"program", program}}).dump());
    refused(executable, "emitter/input-capacity", std::string((16U << 20) + 1, ' '));
    refused(executable, "emitter/depth-capacity", std::string(514, '[') + '0' + std::string(514, ']'));
    refused(executable, "emitter/node-capacity", '[' + repeated("null,", 100000) + "null]");
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 4, "companion tests require source root, installed SDK and fresh proof");
        const fs::path root = argv[1];
        const fs::path sdk = argv[2];
        const fs::path output = argv[3];
        require(root.is_absolute() && sdk.is_absolute() && output.is_absolute() && !fs::exists(output), "companion oracle requires absolute inputs and fresh proof");
        fs::create_directories(output);
        const auto manifest = sdk / "share/pipelang-model/sdk-manifest.json";
        const auto expected = fileHash(manifest);
        verifyManifest(sdk, manifest, expected);
        lexer(sdk / "bin/pipelang-cpp-lexer");
        view(sdk / "bin/pipelang-cpp-lexer", sdk / "bin/pipelang-cpp-view-parser");
        emitter(sdk / "bin/pipelang-cpp-emitter", root, output);
        verifyManifest(sdk, manifest, expected);
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"cases", cases}, {"sdk_manifest_sha256", expected},
            {"scope", "native companion transport/AST/pilot backend; full Core semantic admission and legacy Go differential/export byte parity remain pending"}}));
        std::cout << cases.size() << " native compiler companion cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "compiler companion oracle: " << error.what() << '\n';
        return 1;
    }
}
