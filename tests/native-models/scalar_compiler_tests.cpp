// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/sdk-tools/manifest.hpp"
#include <iostream>

namespace {
using namespace pipelang::tools;
void run(const fs::path& root, const fs::path& sdk, const fs::path& output) {
    const auto compiler = nativeExecutable(sdk / "bin/pipelang-native-cpp");
    const auto emitter = nativeExecutable(sdk / "bin/pipelang-cpp-emitter");
    const auto emitterHash = fileHash(emitter);
    const auto fixture = root / "tests/native-models/ScalarCalls.pipe";
    const auto destination = output / "generated";
    const auto invoke = [&](const fs::path& source, const fs::path& target, const std::string& hash = "", const std::string& pin = "") {
        std::vector<std::string> command{compiler.string(), "--source", source.string(), "--output-dir", target.string(),
            "--profile", "pipelang.cpp.float64-calls.v1", "--package", "example.calls", "--module", "app.calls",
            "--cpp-emitter", emitter.string(), "--cpp-emitter-sha256", hash.empty() ? emitterHash : hash};
        if (!pin.empty()) {
            command.push_back("--source-sha256");
            command.push_back(pin);
        }
        return process(command, {}, std::chrono::seconds(30));
    };
    const auto compiled = invoke(fixture, destination);
    require(compiled.exit == 0 && compiled.output.empty() && compiled.diagnostic.empty(), "native scalar fixture failed: " + compiled.diagnostic);
    const auto metadata = parseJson(readFile(destination / "bindings.json", 1U << 20));
    exactObject(metadata, {"contract", "profile", "language_contract", "compiler_contract", "source_sha256", "generated_sha256",
        "compiler_executable_sha256", "emitter_executable_sha256", "functions"}, {}, "scalar metadata");
    require(metadata.at("contract") == "pipelang.native-cpp-export.v1" && metadata.at("profile") == "pipelang.cpp.float64-calls.v1" &&
        metadata.at("language_contract") == "v0.113.0" && metadata.at("compiler_contract") == "pipelang.compiler.v1" &&
        metadata.at("source_sha256") == fileHash(fixture) && metadata.at("generated_sha256") == fileHash(destination / "generated.hpp") &&
        metadata.at("compiler_executable_sha256") == fileHash(compiler) && metadata.at("emitter_executable_sha256") == emitterHash,
        "scalar metadata identity contract differs");
    const std::map<std::string, std::vector<std::pair<std::string, std::string>>> signatures{
        {"Choose", {{"choose", "bool"}, {"first", "float"}, {"second", "float"}}},
        {"Identity", {{"value", "float"}}}, {"Constant", {}}, {"Zero", {}},
        {"Less", {{"first", "float"}, {"second", "float"}}},
        {"Minimum", {{"first", "float"}, {"second", "float"}}},
        {"Compose", {{"choose", "bool"}, {"first", "float"}, {"second", "float"}}},
        {"Order", {}}, {"Lazy", {{"choose", "bool"}}},
        {"Range", {{"value", "float"}, {"minimum", "float"}, {"maximum", "float"}}},
        {"Coefficients", {{"first", "float"}, {"second", "float"}}}};
    require(metadata.at("functions").size() == signatures.size(), "scalar public function count differs");
    std::map<std::string, std::string> symbols;
    for (const auto& function : metadata.at("functions")) {
        exactObject(function, {"identity", "name", "cpp_symbol", "return_type", "parameters"}, {}, "scalar function");
        const auto name = function.at("name").get<std::string>();
        require(signatures.contains(name) && symbols.emplace(name, function.at("cpp_symbol").get<std::string>()).second, "unexpected or duplicate scalar function");
        auto lower = name;
        for (auto& character : lower) {
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character + ('a' - 'A'));
        }
        const auto path = "app.calls.calls." + lower;
        require(function.at("identity").at("package_id") == "example.calls" && function.at("identity").at("path") == path &&
            function.at("cpp_symbol") == "pipelang::fn_" + sha256(std::string("example.calls\0", 14) + path).substr(0, 24), "scalar semantic identity changed");
        const auto& expected = signatures.at(name);
        require(function.at("parameters").size() == expected.size(), "scalar signature count differs");
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const Json type = expected[index].second == "bool" ? Json{{"kind", "primitive"}, {"primitive", "bool"}} :
                Json{{"kind", "numeric"}, {"numeric", {{"representation", "binary_float"}, {"bits", 64}}}};
            require(function.at("parameters")[index] == Json({{"position", index}, {"name", expected[index].first}, {"type", type}}), "scalar parameter metadata differs");
        }
    }
    std::string runtime = R"cpp(#include "generated.hpp"
#include <array>
#include <cmath>
#include <cstring>
std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
bool nan(std::uint64_t value) {
    return (value & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
        (value & UINT64_C(0x000fffffffffffff)) != 0;
}
bool less(std::uint64_t left, std::uint64_t right) {
    if (nan(left) || nan(right) || (left << 1 == 0 && right << 1 == 0))
        return false;
    const auto order = [](std::uint64_t value) {
        return value & UINT64_C(0x8000000000000000) ? ~value : value ^ UINT64_C(0x8000000000000000);
    };
    return order(left) < order(right);
}
bool same(double actual, std::uint64_t expected) {
    return nan(expected) ? std::isnan(actual) : bits(actual) == expected;
}
int main() {
    const std::array<std::uint64_t, 16> values{
        UINT64_C(0xfff0000000000000), UINT64_C(0xffefffffffffffff), UINT64_C(0xbff0000000000000),
        UINT64_C(0x8010000000000000), UINT64_C(0x8000000000000001), UINT64_C(0x8000000000000000),
        0, 1, UINT64_C(0x0010000000000000), UINT64_C(0x3fefffffffffffff), UINT64_C(0x3ff0000000000000),
        UINT64_C(0x3ff0000000000001), UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0xfff8000000000042)};
    using pipelang::float64_comparison_detail::from_bits;
    for (auto first : values) {
        for (auto second : values) {
)cpp";
    runtime += "            if (!same(" + symbols.at("Minimum") + "(from_bits(first), from_bits(second)), less(second, first) ? second : first))\n                return 1;\n";
    runtime += "            for (bool choose : {false, true}) {\n                if (!same(" + symbols.at("Compose") + "(choose, from_bits(first), from_bits(second)), choose ? first : second))\n                    return 2;\n            }\n";
    runtime += "        }\n    }\n    return " + symbols.at("Zero") + "() == 0.5 && " + symbols.at("Lazy") + "(false) == 0.75 && " + symbols.at("Lazy") + "(true) == 0.25 ? 0 : 3;\n}\n";
    atomicWrite(destination / "runtime.cpp", runtime);
    const auto before = fileHash(destination / "generated.hpp");
    Json refusals = Json::array();
    const std::vector<std::pair<std::string, std::string>> invalid{
        {"arithmetic", "public Class X { public float Add(float x, float y) => x + y; }"},
        {"numeric-negation", "public Class X { public float Negative(float x) => -x; }"},
        {"integer", "public Class X { public int Read(int x) => x; }"},
        {"mixed-arms", "public Class X { public float Read(bool c) => c ? 0.0 : false; }"},
        {"mixed-comparison", "public Class X { public bool Read(float x, bool y) => x == y; }"},
        {"unknown-helper", "public Class X { public float Read(float x) => Missing(x); }"},
        {"argument-type", "public Class X { public float A(float x) => x; public float B() => A(true); }"},
        {"argument-count", "public Class X { public float A(float x) => x; public float B() => A(); }"},
        {"cycle", "public Class X { public float A(float x) => A(x); }"},
        {"local", "public Class X { public float A(float x) { float y = x; return y; } }"},
        {"state-capture", "public Class X { public float value; public float Read() => value; }"},
        {"generic", "public Class X { public T Read<T>(T value) => value; }"}};
    for (const auto& [name, source] : invalid) {
        const auto path = output / (name + ".pipe");
        atomicWrite(path, source);
        const auto result = invoke(path, destination);
        require(result.exit != 0 && result.output.empty(), "scalar source was unexpectedly admitted: " + name);
        require(fileHash(destination / "generated.hpp") == before, "scalar refusal changed previous output");
        refusals.push_back(name);
    }
    require(invoke(fixture, destination, std::string(64, '0')).exit != 0, "scalar emitter identity failure was accepted");
    refusals.push_back("emitter-pin");
    require(invoke(fixture, destination, "", std::string(64, '0')).exit != 0, "scalar source identity failure was accepted");
    refusals.push_back("source-pin");
    const auto unowned = output / "unowned";
    atomicWrite(unowned / "keep.txt", "owned by another producer");
    require(invoke(fixture, unowned).exit != 0 && readFile(unowned / "keep.txt", 128) == "owned by another producer", "scalar compiler replaced unowned outputs");
    refusals.push_back("unowned-output");
    // The runtime source is a test artifact, so validate ownership refusal above
    // before asking the CLI to replace this directory again.
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"public_functions", symbols.size()},
        {"refusals", refusals}, {"independent_selected_value_vectors", 768}, {"runtime_source", (destination / "runtime.cpp").string()}}));
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 4, "scalar tests require source root, installed SDK and fresh proof");
        const fs::path output = argv[3];
        require(output.is_absolute() && !fs::exists(output), "scalar proof must be fresh and absolute");
        fs::create_directories(output);
        run(argv[1], argv[2], output);
        std::cout << "native scalar signatures, hashes, selected-value oracle and 15 refusals passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scalar oracle: " << error.what() << '\n';
        return 1;
    }
}
