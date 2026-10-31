// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/sdk-tools/bindings.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include "windows_junction.hpp"
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using namespace pipelang::tools;
std::size_t refusals = 0;

template<class Operation>
void refuse(Operation operation, const std::string& contract = "invalid input") {
    try {
        operation();
    } catch (const std::exception&) {
        ++refusals;
        return;
    }
    throw std::runtime_error("operation unexpectedly accepted " + contract);
}
Json modelManifest() {
    return {{"protocol", "pipelang.native-model-manifest.v1"}, {"namespace", "fixture"},
            {"records", Json::array()}, {"enums", Json::array()}, {"interfaces", Json::array()},
            {"classes", {{{"name", "Model"}, {"fields", Json::array()}, {"methods", Json::array()},
                          {"constructor", {{"parameters", Json::array()}}}}}}};
}
int fakeTool(bool model) {
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    const auto request = parseJson(bytes);
    const auto source = request.at("source").get<std::string>();
    if (source == "flood") {
        std::cout << std::string((1U << 20) + 1, 'x');
        return 0;
    }
    if (source == "stderr") {
        std::cerr << "unexpected native diagnostic";
        return 0;
    }
    if (source == "duplicate") {
        std::cout << "{\"ok\":true,\"ok\":false}";
        return 0;
    }
    const auto protocol = model ? "pipelang.native-model.v1" : "pipelang.native-model-view.v1";
    if (source == "refuse") {
        std::cout << Json({{"protocol", protocol}, {"ok", false}, {"diagnostics", {{{"message", "fixture refusal"}}}}}).dump();
        return 0;
    }
    Json manifest = model ? modelManifest() : Json({{"protocol", "pipelang.native-model-view-manifest.v1"},
        {"namespace", "fixture"}, {"name", "View"}, {"adapter", "ViewAdapter"}, {"model", "Model"}});
    Json result{{"protocol", protocol}, {"ok", true}, {"header", "// native fixture\n"}, {"manifest", manifest}};
    if (request.contains("style_inspection"))
        result["style_descriptor"] = {{"protocol", "pipelang.native-style-descriptor.v1"}, {"view", "View"},
            {"scope", "resolved-authored"}, {"style", "Compact"},
            {"properties", {{{"name", "gap"}, {"type", "length"}, {"unit", "px"}, {"value", 4.5}}}}};
    std::cout << result.dump();
    return 0;
}

void primitiveContracts(const fs::path& root, const fs::path& self) {
    require(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA256 vector failed");
    require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 abc vector failed");
    const std::string million(1000000, 'a');
    require(sha256(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "streaming SHA256 vector failed");
    atomicWrite(root / "hash-input", million);
    require(fileHash(root / "hash-input") == sha256(million), "streaming file hash differs");
    refuse([] { parseJson("{\"a\":1,\"a\":2}"); });
    refuse([] { parseJson("{\"a\":NaN}"); });
    refuse([] { parseJson(std::string(66, '[') + "0" + std::string(66, ']')); });
    for (const auto& name : {"../bad", "/bad", "a/../b", "a//b", "a\\b", "a/", "C:bad"})
        refuse([&] { portablePath(name); }, "portable path: " + std::string(name));
    refuse([&] { process({self.string(), "--sleep"}, {}, std::chrono::milliseconds(30)); }, "process deadline");
    refuse([&] { process({self.string(), "--flood"}, {}, std::chrono::seconds(2), 128); }, "process output limit");
    const auto result = process({self.string(), "--echo"}, "literal $(not-a-command) `text`\n");
    require(result.exit == 0 && result.output == "literal $(not-a-command) `text`\n" && result.diagnostic.empty(), "process transport changed literal data");
}

void admissionAndPublication(const fs::path& root, const fs::path& self) {
    const auto prefix = root / "fake-sdk";
    fs::create_directories(prefix / "bin");
    const auto model = nativeExecutable(prefix / "bin/fake-model");
    const auto view = nativeExecutable(prefix / "bin/fake-view");
    fs::copy_file(self, model);
    fs::copy_file(self, view);
    const auto manifest = prefix / "sdk.json";
    recordManifest(prefix, manifest, Json::array({"bin"}));
    const auto hash = fileHash(manifest);
    verifyManifest(prefix, manifest, hash);
    refuse([&] { verifyManifest(prefix, manifest, std::string(64, '0')); });
    atomicWrite(prefix / "bin/extra", "unmanifested");
    refuse([&] { verifyManifest(prefix, manifest, hash); });
    fs::remove(prefix / "bin/extra");
#ifdef _WIN32
    const auto junctionTarget = root / "junction-target";
    atomicWrite(junctionTarget / "protected", "outside SDK roots");
    pipelang::tests::DirectoryJunction junction(prefix / "bin/symbolic", junctionTarget);
    refuse([&] { recordManifest(prefix, manifest, Json::array({"bin"})); }, "junction input");
    junction.remove();
    require(readFile(junctionTarget / "protected", 128) == "outside SDK roots", "junction fixture changed its target");
#else
    fs::create_symlink(model, prefix / "bin/symbolic");
    refuse([&] { recordManifest(prefix, manifest, Json::array({"bin"})); });
    fs::remove(prefix / "bin/symbolic");
#endif
    const auto modelSource = root / "model.pipe";
    const auto viewSource = root / "view.pipe";
    atomicWrite(modelSource, "model");
    atomicWrite(viewSource, "view");
    const auto output = root / "bindings";
    const auto generate = [&](bool inspection) {
        return generateBindings(prefix, manifest, hash, model, view, modelSource, viewSource, "fixture", output, inspection);
    };
    const auto first = generate(true);
    require(first.at("generated").contains("style.json") && fs::exists(output / "style.json"), "style output missing");
    const auto second = generate(false);
    require(!second.at("generated").contains("style.json") && !fs::exists(output / "style.json"), "stale style output survived publication");
    std::map<std::string, std::string> before;
    for (const auto& entry : fs::directory_iterator(output))
        before[entry.path().filename().string()] = readFile(entry.path(), 1U << 20);
    for (const auto& invalid : {"refuse", "flood", "stderr", "duplicate"}) {
        atomicWrite(modelSource, invalid);
        refuse([&] { generate(false); });
        for (const auto& [name, bytes] : before)
            require(readFile(output / name, 1U << 20) == bytes, "failed generation modified published artifacts");
    }
    atomicWrite(modelSource, "model");
    atomicWrite(output / "user-owned", "protected");
    refuse([&] { generate(false); });
    require(readFile(output / "user-owned", 64) == "protected", "unowned output was modified");
    auto bad = modelManifest();
    bad["unexpected"] = true;
    refuse([&] { admitModel(bad, "fixture"); });
    bad = modelManifest();
    bad["classes"][0]["methods"] = {{{"name", "Read"}, {"returnType", "int"}, {"parameters", Json::array()}, {"pure", 1}, {"async", false}}};
    refuse([&] { admitModel(bad, "fixture"); });
}

void installedSdk(const fs::path& source, const fs::path& prefix, const fs::path& output) {
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto hash = fileHash(manifest);
    const auto model = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto view = nativeExecutable(prefix / "bin/pipelang-model-viewc");
    const auto generated = output / "installed-bindings";
    generateBindings(prefix, manifest, hash, model, view,
        source / "tests/native-model-views/FixtureModel.pipe", source / "tests/native-model-views/Fixture.pipe",
        "native_tooling", generated, true);
    require(fs::exists(generated / "Model.hpp") && fs::exists(generated / "ModelView.hpp") &&
            fs::exists(generated / "style.json"), "installed compiler outputs missing");
    const auto consumer = output / "consumer";
    fs::create_directory(consumer);
    atomicWrite(consumer / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(NativeSdkConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model_bindings(bindings MODEL "${MODEL_SOURCE}" VIEW "${VIEW_SOURCE}"
    NAMESPACE native_tooling OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated" STYLE_INSPECTION)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE bindings)
)cmake");
    atomicWrite(consumer / "main.cpp", R"cpp(#include "Model.hpp"
int main() {
    native_tooling::Values value;
    native_tooling::FixtureModel model(value);
    return model.snapshot().Draft.Volume == 50 ? 0 : 1;
}
)cpp");
    const auto invoke = [&](const std::vector<std::string>& command) {
        const auto result = process(command, {}, std::chrono::seconds(90));
        require(result.exit == 0, "installed SDK consumer failed: " + result.output + result.diagnostic);
    };
    invoke({nativeTool("cmake").string(), "-S", consumer.string(), "-B", (consumer / "build").string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
            "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DMODEL_SOURCE=" + (source / "tests/native-model-views/FixtureModel.pipe").string(),
            "-DVIEW_SOURCE=" + (source / "tests/native-model-views/Fixture.pipe").string()});
    invoke({nativeTool("cmake").string(), "--build", (consumer / "build").string(), "--parallel", "1"});
    invoke({nativeExecutable(consumer / "build/consumer").string()});
}
}

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        const auto name = fs::path(argv[0]).stem().string();
        if (name == "fake-model" || name == "fake-view")
            return fakeTool(name == "fake-model");
        if (argc == 2 && std::string(argv[1]) == "--sleep") {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--flood") {
            std::cout << std::string(4096, 'x');
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--echo") {
            std::cout << std::cin.rdbuf();
            return 0;
        }
        require(argc == 4, "usage: sdk-tests SOURCE_ROOT INSTALLED_SDK OUTPUT");
        const fs::path output = argv[3];
        require(!fs::exists(output), "test output must be fresh");
        fs::create_directories(output);
        const auto self = fs::canonical(argv[0]);
        primitiveContracts(output, self);
        admissionAndPublication(output, self);
        installedSdk(argv[1], argv[2], output);
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"refusals", refusals},
            {"native_sdk_install_and_consume", true}, {"python_discovery_disabled", true}}));
        std::cout << refusals << " native SDK/tooling refusals; installed consumer passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
