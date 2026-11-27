// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"
#include "../../src/native/model-compiler/CompiledImplementationUnits.hpp"
#include "../../src/native/lexer/lexer.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include "../../src/native/sdk-tools/source_set.hpp"
#include "../../src/native/sdk-tools/ResourceBudgetReceipt.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace pipelang::models;
using namespace pipelang::tools;
std::size_t checks = 0;
void check(bool condition, const std::string& message) {
    require(condition, message);
    ++checks;
}
template<class Operation>
void rejects(Operation operation, const std::string& message) {
    try {
        operation();
    } catch (const std::exception& error) {
        check(std::string(error.what()).find(message) != std::string::npos,
              "refusal omitted resource/correctness cause: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected refusal: " + message);
}
void budgetValuesAndScopes() {
    for (const auto& text : {"[]", "{\"tokens\":0}", "{\"tokens\":-1}", "{\"tokens\":1.5}",
                            "{\"tokens\":true}", "{\"tokens\":18446744073709551616}",
                            "{\"unknown\":1}", "{\"tokens\":1,\"tokens\":2}"})
        rejects([&] { (void)ResourceBudget::fromText(text); }, "");
    const auto defaults = resourceBudget();
    check(!budgetExceeded(9, 1, 10) && budgetExceeded(10, 1, 10) &&
          budgetExceeded(std::numeric_limits<std::size_t>::max(), 1, std::numeric_limits<std::size_t>::max()),
          "checked size_t budget accounting");
    {
        auto outer = defaults;
        outer.declarations = 512;
        ScopedResourceBudget scope(outer);
        try {
            auto inner = outer;
            inner.tokens = 4;
            ScopedResourceBudget nested(inner);
            throw std::runtime_error("fixture unwind");
        } catch (const std::runtime_error&) {}
        check(resourceBudget() == outer, "nested budget did not restore on failure");
        auto invalid = outer;
        invalid.tokens = 0;
        rejects([&] { ScopedResourceBudget failed(invalid); }, "positive");
        check(resourceBudget() == outer, "failed budget constructor changed the active profile");
        std::atomic<bool> independent{false};
        std::thread worker([&] { independent = resourceBudget() == defaults; });
        worker.join();
        check(independent, "budget profile leaked across threads");
    }
    check(resourceBudget() == defaults, "outer budget did not restore");
}
void compilerAdmissionAndCorrectness() {
    const std::string record = "public Record Value { public int number = 1; }";
    for (std::size_t bytes : {65535U, 65537U, 131072U}) {
        auto source = std::string(bytes - record.size(), ' ') + record;
        check(compile(source, "budget_single").at("ok") == true, "large individual source rejected");
    }
    auto low = resourceBudget();
    low.inputBytes = 16;
    {
        ScopedResourceBudget scope(low);
        rejects([&] { (void)compile(record, "input_budget"); }, "input_bytes");
    }
    low = resourceBudget();
    low.outputBytes = 16;
    {
        ScopedResourceBudget scope(low);
        rejects([&] { (void)compile(record, "output_budget"); }, "output_bytes");
    }
    std::vector<ModuleSource> parts;
    for (int index = 0; index < 200; ++index) {
        parts.push_back({"Many", "module Many; public Record R" + std::to_string(index) +
                         " { public int value = " + std::to_string(index) + "; }", "R" + std::to_string(index) + ".pipe"});
    }
    rejects([&] { (void)compileProjectModules(parts, "Many", "many"); }, "declaration");
    auto raised = resourceBudget();
    raised.declarations = 512;
    {
        ScopedResourceBudget scope(raised);
        check(compileProjectModules(parts, "Many", "many").at("ok") == true, "configured declarations did not admit the project");
        parts.push_back({"Many", "module Many; public Record Box<T> { public T value; } public Record Use { public Box<int> box; }", "Generic.pipe"});
        check(compileProjectModules(parts, "Many", "many").at("ok") == true, "generic expansion retained a hidden declaration ceiling");
        parts.push_back(parts.front());
        parts.back().path = "Duplicate.pipe";
        rejects([&] { (void)compileProjectModules(parts, "Many", "many"); }, "duplicate");
    }
    std::string late = "module Late;\n" + std::string(80000, ' ') + "public Record Bad { public Unknown value; }";
    try {
        (void)compileProjectModules({{"Late", late, "late/Late.pipe"}}, "Late", "late");
        throw std::runtime_error("late type error accepted");
    } catch (const Diagnostic& error) {
        check(error.path == "late/Late.pipe" && error.offset >= 80000, "late type diagnostic lost its source identity/offset");
    }
    late = "module Late;\n" + std::string(80000, ' ') + "@";
    try {
        (void)compileProjectModules({{"Late", late, "late/Late.pipe"}}, "Late", "late");
        throw std::runtime_error("late lexical error accepted");
    } catch (const Diagnostic& error) {
        check(error.path == "late/Late.pipe" && error.offset == late.size() - 1, "late lexer diagnostic offset truncated");
    }
    const auto flooded = pipelang::native::lex(std::string(1024 * 1024, ';'), pipelang::native::NumericSyntax::Binary64, 8);
    check(flooded.failure && flooded.failure->start == 7, "lexer did not enforce token allocation budget early");
    const auto repeated = std::string(128 * 1024, ' ') + record;
    for (int index = 0; index < 20; ++index)
        check(compile(repeated, "repeated").at("ok") == true, "repeated large source compilation changed behavior");
    auto unitsBudget = resourceBudget();
    unitsBudget.implementationUnits = 300;
    {
        ScopedResourceBudget scope(unitsBudget);
        CompiledImplementationUnits units("unit_budget");
        for (int index = 0; index < 300; ++index)
            units.addOwner("Owner" + std::to_string(index));
        check(units.files().size() == 300, "compiled implementation retained its hidden 256 unit ceiling");
        rejects([&] { units.addOwner("Excess"); }, "implementation_units");
    }
}
void sdkAdmission(const fs::path& root) {
    fs::create_directories(root / "src");
    atomicWrite(root / "src/Entry.pipe", "module Entry; import Data; public Record Request { public Value value; }");
    atomicWrite(root / "src/Data.pipe", "module Data;\n" + std::string(80000, ' ') + "public Record Value { public int value; }");
    const auto project = SourceProject::load(root, "src", "Entry.pipe");
    check(project.dependencies().size() == 2, "SDK rejected large individual file/closure");
    project.verifyUnchanged();
    auto low = resourceBudget();
    low.indexedBytes = 65536;
    {
        ScopedResourceBudget scope(low);
        rejects([&] { (void)SourceProject::load(root, "src", "Entry.pipe"); }, "indexed_bytes");
    }
    atomicWrite(root / "src/Unused.pipe", "module Unused; @");
    rejects([&] { (void)SourceProject::load(root, "src", "Entry.pipe"); }, "offset");
    fs::remove(root / "src/Unused.pipe");
    atomicWrite(root / "sources.json", canonical(Json{{"protocol", "pipelang.native-model-source-set.v1"}, {"entry", "Entry"},
        {"modules", Json::array({{{"name", "Entry"}, {"path", "src/Entry.pipe"}}, {{"name", "Data"}, {"path", "src/Data.pipe"}}})}}));
    check(SourceSet::load(root / "sources.json").dependencies().size() == 3, "source-set loader retained a byte ceiling");
    low = resourceBudget();
    low.outputBytes = 16;
    {
        ScopedResourceBudget scope(low);
        rejects([&] { (void)SourceSet::load(root / "sources.json").information(); }, "output_bytes");
    }
    const auto profile = root / "limits.json";
    atomicWrite(profile, "{\"declarations\":512}");
    {
        ScopedResourceBudget scope(ResourceBudget::fromText(readFile(profile, 65536)), profile.string(), fileHash(profile));
        check(resourceBudgetReceipt().at("source_sha256") == fileHash(profile), "profile source hash missing");
        atomicWrite(profile, "{\"declarations\":256}");
        rejects([&] { (void)resourceBudgetReceipt(); }, "changed during compilation");
    }
}

void sdkCmakeProfile(const fs::path& prefix, const fs::path& root) {
    require(!fs::exists(root), "SDK profile fixture must be fresh");
    fs::create_directories(root / "src");
    std::string source = "module Profile;\n";
    for (int index = 0; index < 200; ++index)
        source += "public Record R" + std::to_string(index) + " { public int value = 1; }\n";
    atomicWrite(root / "src/Entry.pipe", source);
    const auto profile = root / "limits.json";
    atomicWrite(profile, "{\"declarations\":128}\n");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21)
project(ResourceProfileConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(profile_model PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src ENTRY Entry.pipe NAMESPACE resource_profile
    RESOURCE_BUDGET "${CMAKE_CURRENT_SOURCE_DIR}/limits.json"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
)cmake");
    const auto cmake = nativeTool("cmake").string();
    auto configured = process({cmake, "-S", root.string(), "-B", (root / "build").string(),
                               "-DCMAKE_PREFIX_PATH=" + prefix.string()}, {}, std::chrono::seconds(60), 4U << 20);
    check(configured.exit == 0, "profile CMake configure failed: " + configured.diagnostic);
    auto build = [&] {
        return process({cmake, "--build", (root / "build").string(), "--parallel", "1", "--target", "profile_model_generate"},
                       {}, std::chrono::seconds(60), 4U << 20);
    };
    auto refused = build();
    check(refused.exit != 0 && (refused.output + refused.diagnostic).find("declaration") != std::string::npos,
          "low profile did not reject generated declarations");
    check(!fs::exists(root / "build/generated/bindings.json"), "failed profile published partial output");
    atomicWrite(profile, "{\"declarations\":512}\n");
    const auto accepted = build();
    check(accepted.exit == 0, "profile dependency did not rerun generation: " + accepted.diagnostic);
    const auto receiptPath = root / "build/generated/bindings.json";
    const auto receiptBytes = readFile(receiptPath, 4U << 20);
    const auto receipt = parseJson(receiptBytes, 4U << 20);
    check(receipt.at("resource_budget").at("profile").at("declarations") == 512 &&
          receipt.at("resource_budget").at("source_sha256") == fileHash(profile), "profile receipt/hash mismatch");
    const auto manifest = parseJson(readFile(root / "build/generated/model.json", 4U << 20), 4U << 20);
    check(manifest.at("records").size() == 200, "configured SDK compilation truncated source declarations");
    atomicWrite(profile, "{\"declarations\":512,\"tokens\":1}\n");
    refused = build();
    check(refused.exit != 0 && (refused.output + refused.diagnostic).find("tokens") != std::string::npos,
          "changed low token profile was not re-admitted");
    check(readFile(receiptPath, 4U << 20) == receiptBytes, "refused changed profile replaced prior accepted output");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--sdk") {
            sdkCmakeProfile(fs::absolute(argv[2]), fs::absolute(argv[3]));
            std::cout << "SDK/CMake resource profile contracts passed: " << checks << '\n';
            return 0;
        }
        require(argc == 2, "resource budget tests need an owned fixture root");
        const auto root = fs::absolute(argv[1]);
        require(!fs::exists(root), "resource budget fixture root must be fresh");
        budgetValuesAndScopes();
        compilerAdmissionAndCorrectness();
        sdkAdmission(root);
        std::cout << "compiler resource budget contracts passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "compiler resource budget test failure: " << error.what() << '\n';
        return 1;
    }
}
