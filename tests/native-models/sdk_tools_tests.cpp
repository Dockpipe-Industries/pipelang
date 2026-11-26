// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/sdk-tools/bindings.hpp"
#include "../../src/native/sdk-tools/source_set.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"
#include "ProjectTests.hpp"
#include "BuildTimestamp.h"
#include "NativeNumericExportTests.hpp"
#include "CompiledProjectTests.hpp"
#include "CompiledOwnerFixture.hpp"
#include "LinkedRuntimeFixture.hpp"
#include "ProjectModuleTests.hpp"
#include <chrono>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include "windows_junction.hpp"
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace pipelang::tools;
std::size_t refusals = 0;

template<class Operation>
void refuse(Operation operation, const std::string& contract = "invalid input",
            const std::string& expectedMessage = {}) {
    try {
        operation();
    } catch (const std::exception& error) {
        require(expectedMessage.empty() || std::string(error.what()).find(expectedMessage) != std::string::npos,
                "refusal did not explain " + contract + ": " + error.what());
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
void staticMethodManifestContracts() {
    const Json method{{"name", "Read"}, {"returnType", "int"}, {"parameters", Json::array()},
        {"pure", true}, {"async", false}, {"static", true}};
    auto legacy = modelManifest();
    admitModel(legacy, "fixture");
    legacy["classes"][0]["methods"] = Json::array({method});
    refuse([&] { admitModel(legacy, "fixture"); }, "static metadata in legacy model manifest");

    auto modern = legacy;
    modern["protocol"] = "pipelang.native-model-manifest.v2";
    modern["objects"] = Json::array();
    modern["sourceInterfaces"] = Json::array();
    admitModel(modern, "fixture");
    modern["classes"][0]["methods"][0]["static"] = false;
    admitModel(modern, "fixture");

    for (const auto& invalidFlag : {Json(1), Json("true"), Json(nullptr), Json::array()}) {
        auto invalid = modern;
        invalid["classes"][0]["methods"][0]["static"] = invalidFlag;
        refuse([&] { admitModel(invalid, "fixture"); }, "non-boolean static metadata", "exact boolean");
    }

    for (const auto& category : {"interfaces", "sourceInterfaces"}) {
        auto invalid = modern;
        invalid[category] = Json::array({Json{{"name", "Reader"}, {"methods", Json::array({method})}}});
        refuse([&] { admitModel(invalid, "fixture"); }, "static metadata on an interface");
    }

    auto object = modern;
    object["objects"] = Json::array({Json{{"name", "Object"}, {"fields", Json::array()},
        {"methods", Json::array({method})}, {"constructor", {{"parameters", Json::array()}}},
        {"implements", Json::array()}}});
    admitModel(object, "fixture");

    auto duplicate = modern;
    duplicate["classes"][0]["methods"].push_back(method);
    refuse([&] { admitModel(duplicate, "fixture"); }, "duplicate static/instance method signature");
    std::cout << "static method manifest admission contracts passed\n";
}

int fakeTool(bool model) {
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    const auto request = parseJson(bytes);
    const auto source = request.contains("source") ? request.at("source").get<std::string>()
        : request.at("modules").at(0).at("source").get<std::string>();
    if (source.starts_with("mutate-source:"))
        atomicWrite(fs::path(source.substr(14)), "changed while compiling");
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
    const auto protocol = model ? request.at("protocol").get<std::string>() : "pipelang.native-model-view.v1";
    if (source == "refuse") {
        std::cout << Json({{"protocol", protocol}, {"ok", false}, {"diagnostics", {{{"message", "fixture refusal"}}}}}).dump();
        return 0;
    }
    Json manifest = model ? modelManifest() : Json({{"protocol", "pipelang.native-model-view-manifest.v1"},
        {"namespace", "fixture"}, {"name", "View"}, {"adapter", "ViewAdapter"}, {"model", "Model"}});
    if (model && protocol == "pipelang.native-model.v3") {
        manifest["protocol"] = "pipelang.native-model-manifest.v2";
        manifest["objects"] = Json::array();
        manifest["sourceInterfaces"] = Json::array();
    }
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
#ifndef _WIN32
    const auto expectExecFailure = [&](const fs::path& executable, int expectedError, const fs::path& log = {}) {
        const auto failed = process({executable.string()}, {}, std::chrono::seconds(2), 65536, 65536, {}, log);
        const auto expected = "native process exec failed (errno " + std::to_string(expectedError) + ")\n";
        require(failed.exit == 127 && failed.output.empty() && failed.diagnostic == expected,
                "executable failure lost its exit status or errno diagnostic: " + failed.diagnostic);
        if (!log.empty())
            require(readFile(log, 65536) == expected, "executable failure was not captured in its process log");
    };
    const auto absent = root / "absent-process-executable";
    require(!fs::exists(absent), "missing-executable fixture must be absent");
    expectExecFailure(absent, ENOENT, root / "missing-process.log");
    const auto invalidExecutable = root / "invalid-process-executable";
    atomicWrite(invalidExecutable, "not an executable format\n");
    fs::permissions(invalidExecutable, fs::perms::owner_read | fs::perms::owner_write);
    expectExecFailure(invalidExecutable, EACCES);
    fs::permissions(invalidExecutable, fs::perms::owner_all);
    expectExecFailure(invalidExecutable, ENOEXEC);
    refuse([&] { process({absent.string()}, {}, std::chrono::seconds(2), 65536, 1); },
            "executable diagnostic capacity", "output exceeds its capacity");
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto startup = process({self.string(), "--process-group"}, {}, std::chrono::seconds(2));
        require(startup.exit == 0 && startup.diagnostic.empty(),
                "private process-group startup failed on attempt " + std::to_string(attempt)
                    + ": " + startup.diagnostic);
        const auto identity = parseJson(startup.output);
        require(identity.is_array() && identity.size() == 2,
                "private process-group child returned an invalid identity");
        const auto childPid = identity.at(0).get<pid_t>();
        const auto childGroup = identity.at(1).get<pid_t>();
        require(childPid > 0 && childGroup == childPid && childGroup != ::getpgrp(),
                "native child did not execute in its own private process group");
    }
#endif
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
    const auto viewSource = root / "view.pview";
    atomicWrite(modelSource, "model");
    atomicWrite(viewSource, "view");
    const auto output = root / "bindings";
    const auto generate = [&](bool inspection) {
        return generateBindings(prefix, manifest, hash, model, view, modelSource, viewSource, "fixture", output, inspection);
    };
    const auto first = generate(true);
    require(first.at("generated").contains("style.json") && fs::exists(output / "style.json"), "style output missing");
    const auto publishedView = readFile(output / "ModelView.hpp", 1U << 20);
    const auto publishedReceipt = readFile(output / "bindings.json", 1U << 20);
    for (const auto& [extension, diagnostic] : std::vector<std::pair<std::string, std::string>>{
        {".pipe", "ordinary MODEL sources retain .pipe"},
        {".vpipe", ".vpipe is not a PipeView source extension"},
        {".xml", "VIEW source must have the .pview or .pxml extension"}}) {
        const auto rejectedView = root / ("view" + extension);
        atomicWrite(rejectedView, "view");
        refuse([&] {
            generateBindings(prefix, manifest, hash, model, view, modelSource, rejectedView,
                             "fixture", output, true);
        }, "view extension " + extension, diagnostic);
        require(readFile(output / "ModelView.hpp", 1U << 20) == publishedView &&
                readFile(output / "bindings.json", 1U << 20) == publishedReceipt,
                "refused view extension modified the published view");
    }
    const auto second = generate(false);
    require(!second.at("generated").contains("style.json") && !fs::exists(output / "style.json"), "stale style output survived publication");
    const auto headless = generateModelBindings(prefix, manifest, hash, model, modelSource, "fixture", output);
    require(headless.at("protocol") == "pipelang.native-model-artifacts.v1" &&
            !headless.contains("view_source_sha256") && !headless.at("tools").contains("view"),
        "headless model receipt depends on a view");
    require(fs::exists(output / "Model.hpp") && fs::exists(output / "model.json") &&
            !fs::exists(output / "ModelView.hpp") && !fs::exists(output / "view.json"),
        "headless publication retained stale view artifacts");
    require(!headless.at("generated").contains("Types.hpp") && !fs::exists(output / "Types.hpp"),
            "legacy publication emitted project-only types");
    refuse([&] { publishDirectory(output, {{"Types.hpp.user", "unowned"}}); });
    require(!fs::exists(output / "Types.hpp.user"), "publication admitted a near-match filename");
    std::map<std::string, std::string> before;
    for (const auto& entry : fs::directory_iterator(output))
        before[entry.path().filename().string()] = readFile(entry.path(), 1U << 20);
    for (const auto& invalid : {"refuse", "flood", "stderr", "duplicate"}) {
        atomicWrite(modelSource, invalid);
        refuse([&] { generate(false); });
        refuse([&] { generateModelBindings(prefix, manifest, hash, model, modelSource, "fixture", output); });
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
    auto constantModel = modelManifest();
    const auto owner = constantModel.at("classes")[0].at("name").get<std::string>();
    constantModel["constants"] = {{{"module", ""}, {"owner", owner}, {"name", "Gravity"},
        {"type", "float32"}, {"encoding", "ieee754-binary32-bits"},
        {"cpp_name", "fixture::constants::" + owner + "::Gravity"}, {"value", "1148534784"}}};
    admitModel(constantModel, "fixture");
    for (const auto& key : {"cpp_name", "encoding", "owner", "value"}) {
        auto invalid = constantModel;
        invalid["constants"][0][key] = "invalid";
        refuse([&] { admitModel(invalid, "fixture"); }, "invalid constant metadata admitted");
    }
    auto oversizedConstant = constantModel;
    oversizedConstant["constants"][0]["value"] = "4294967296";
    refuse([&] { admitModel(oversizedConstant, "fixture"); }, "oversized binary32 bits admitted");
    auto binary32 = modelManifest();
    binary32["classes"][0]["fields"] = {{{"name", "value"}, {"type", "float32"}}};
    admitModel(binary32, "fixture");
    refuse([&] { requireViewCompatible(binary32); }, "binary32 model in unsupported view bridge");
    auto unsignedModel = modelManifest();
    unsignedModel["classes"][0]["fields"] = {{{"name", "word"}, {"type", "uint32"}}};
    admitModel(unsignedModel, "fixture");
    refuse([&] { requireViewCompatible(unsignedModel); }, "uint32 model in unsupported view bridge");
    unsignedModel["classes"][0]["fields"][0]["type"] = "List<uint32>";
    admitModel(unsignedModel, "fixture");
    refuse([&] { requireViewCompatible(unsignedModel); }, "nested uint32 in unsupported view bridge");
    fs::remove(view);

    recordManifest(prefix, manifest, Json::array({"bin"}));
    const auto modelOnlyHash = fileHash(manifest);
    generateModelBindings(prefix, manifest, modelOnlyHash, model, modelSource, "fixture", root / "model-only");
    require(fs::exists(root / "model-only/Model.hpp"), "headless SDK requires an unselected view compiler");
    refuse([&] {
        generateModelBindings(prefix, manifest, modelOnlyHash, self, modelSource, "fixture", root / "unpinned-model");
    }, "model compiler outside the pinned SDK");
}

void sourceSetAdmission(const fs::path& root, const fs::path& self) {
    const auto prefix = root / "source-set-sdk";
    fs::create_directories(prefix / "bin");
    const auto model = nativeExecutable(prefix / "bin/fake-model");
    fs::copy_file(self, model);
    const auto sdkManifest = prefix / "sdk.json";
    recordManifest(prefix, sdkManifest, Json::array({"bin"}));
    const auto sdkHash = fileHash(sdkManifest);
    const auto directory = root / "source-set-inputs";
    const auto descriptor = directory / "models.sources.json";
    const auto app = directory / "App.pipe";
    const auto values = directory / "values/Position.pipe";
    atomicWrite(app, "module App; import Values; public Class Model { }");
    atomicWrite(values, "module Values; public Record Position { public int x = 1; }");
    const Json declared{{"protocol", "pipelang.native-model-source-set.v1"}, {"entry", "App"},
        {"modules", {{{"name", "Values"}, {"path", "values/Position.pipe"}},
                      {{"name", "App"}, {"path", "App.pipe"}}}}};
    atomicWrite(descriptor, canonical(declared));
    const auto admitted = SourceSet::load(descriptor);
    require(admitted.compilerProtocol() == "pipelang.native-model.v2" &&
            admitted.dependencies().size() == 3, "closed source-set inputs were not admitted");
    const auto request = admitted.request("fixture");
    require(!request.contains("source") && !request.contains("models") &&
            request.at("modules").at(0).at("name") == "App", "source-set request was concatenated or unordered");
    const auto info = sourceSetInformation(prefix, sdkManifest, sdkHash, descriptor);
    require(info.at("protocol") == "pipelang.native-model-source-set-info.v1" &&
            info.at("dependencies").size() == 3, "source-set dependency information is incomplete");
    const auto output = root / "source-set-bindings";
    const auto generate = [&] {
        return generateSourceSetBindings(prefix, sdkManifest, sdkHash, model, descriptor, "fixture", output);
    };
    const auto first = generate();
    require(first.at("protocol") == "pipelang.native-model-artifacts.v2" &&
            !first.contains("model_source_sha256") && first.at("source_set") == admitted.receipt() &&
            first.at("tools").at("model").at("protocol") == "pipelang.native-model.v2",
            "source-set receipt lost its closed input or compiler identity");
    require(generate() == first, "unchanged source-set generation is not reproducible");
    std::map<std::string, std::string> before;
    for (const auto& entry : fs::directory_iterator(output))
        before[entry.path().filename().string()] = readFile(entry.path(), 1U << 20);
    const auto unchangedOutput = [&] {
        for (const auto& [name, bytes] : before)
            require(readFile(output / name, 1U << 20) == bytes, "refused source set changed published output");
    };
    std::vector<Json> invalid;
    auto changed = declared;
    changed["unexpected"] = true;
    invalid.push_back(changed);
    changed = declared;
    changed["entry"] = "Missing";
    invalid.push_back(changed);
    changed = declared;
    const Json duplicateModule = changed["modules"].at(0);
    changed["modules"].push_back(duplicateModule);
    invalid.push_back(changed);
    changed = declared;
    changed["modules"][0]["name"] = "Different";
    changed["modules"][0]["path"] = "App.pipe";
    invalid.push_back(changed);
    for (const auto& badPath : {"../outside.pipe", "a//b.pipe", "C:bad", "a\\b.pipe", "missing.pipe", "a;b.pipe"}) {
        changed = declared;
        changed["modules"][0]["path"] = badPath;
        invalid.push_back(changed);
    }
    for (const auto& exports : {Json(nullptr), Json::array(), Json::array({"Model", "Model"})}) {
        changed = declared;
        changed["models"] = exports;
        invalid.push_back(changed);
    }
    changed = declared;
    changed["modules"] = Json::array();
    invalid.push_back(changed);
    changed = declared;
    changed["modules"] = Json(nullptr);
    invalid.push_back(changed);
    changed = declared;
    for (int index = 0; index < 129; ++index)
        changed["modules"].push_back({{"name", "OverLimit"}, {"path", "App.pipe"}});
    invalid.push_back(changed);
    for (const auto& bad : invalid) {
        atomicWrite(descriptor, canonical(bad));
        refuse([&] { generate(); }, "invalid source-set descriptor");
        unchangedOutput();
    }
    atomicWrite(descriptor, "{\"protocol\":1,\"protocol\":2}");
    refuse([&] { generate(); }, "duplicate source-set descriptor JSON key");
    unchangedOutput();
    atomicWrite(descriptor, std::string(65537, ' '));
    refuse([&] { generate(); }, "source-set descriptor byte bound");
    unchangedOutput();
    atomicWrite(descriptor, canonical(declared));
    atomicWrite(values, "changed after admission");
    refuse([&] { admitted.verifyUnchanged(); }, "dependency changed after snapshot");
    atomicWrite(values, "module Values; public Record Position { public int x = 1; }");
    atomicWrite(app, "mutate-source:" + values.string());
    refuse([&] { generate(); }, "dependency changed during compiler execution");
    unchangedOutput();
    atomicWrite(app, "module App; import Values; public Class Model { }");
    atomicWrite(values, "module Values; public Record Position { public int x = 1; }");
    refuse([&] {
        generateSourceSetBindings(prefix, sdkManifest, sdkHash, model, descriptor, "fixture", directory);
    }, "output directory owns source-set inputs");
    changed = declared;
    changed["models"] = Json::array({"Model", "Values.Other"});
    atomicWrite(descriptor, canonical(changed));
    const auto objects = generate();
    require(objects.at("source_set").at("models") == changed.at("models") &&
            objects.at("tools").at("model").at("protocol") == "pipelang.native-model.v3",
            "explicit source model exports did not select the object protocol");
    atomicWrite(descriptor, canonical(declared));
    atomicWrite(values, "module Values;\n" + std::string(65536, ' ') +
                        "public Record Position { public int x = 1; }");
    require(generate().contains("source_set"), "source-set retained the obsolete source-byte ceiling");
    {
        auto budget = pipelang::models::resourceBudget();
        budget.inputBytes = 65536;
        const pipelang::models::ScopedResourceBudget scopedBudget(budget);
        refuse([&] { generate(); }, "explicit source-set input resource budget");
    }
    atomicWrite(values, "module Values; public Record Position { public int x = 1; }");
#ifndef _WIN32
    const auto actual = directory / "actual-values";
    fs::rename(directory / "values", actual);
    fs::create_directory_symlink(actual, directory / "values");
    refuse([&] { generate(); }, "symbolic module ancestor");
    fs::remove(directory / "values");
    fs::rename(actual, directory / "values");
#else
    const auto actual = directory / "actual-values";
    fs::rename(directory / "values", actual);
    pipelang::tests::DirectoryJunction junction(directory / "values", actual);
    refuse([&] { generate(); }, "junction module ancestor");
    junction.remove();
    fs::rename(actual, directory / "values");
#endif
    atomicWrite(output / "user-owned", "protected material");
    refuse([&] { generate(); }, "source-set publication contains unowned files");
    require(readFile(output / "user-owned", 128) == "protected material", "source-set publication removed user material");
}

void installedSourceSet(const fs::path& source, const fs::path& prefix, const fs::path& output) {
    const auto directory = output / "source-set-consumer";
    const auto checkout = directory / "source";
    const auto build = directory / "build";
    fs::create_directories(directory);
    fs::copy(source / "tests/sdk-source-set-consumer", checkout, fs::copy_options::recursive);
    const auto invoke = [&](const std::vector<std::string>& command) {
        const auto result = process(command, {}, std::chrono::seconds(90));
        require(result.exit == 0, "installed source-set consumer failed: " + result.output + result.diagnostic);
    };
    invoke({nativeTool("cmake").string(), "-S", checkout.string(), "-B", build.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
            "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    const std::vector<std::string> compile{nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"};
    const auto consumer = nativeExecutable(build / "pipelang-source-set-consumer");
    const auto descriptor = checkout / "models/game.sources.json";
    const auto app = checkout / "models/App.pipe";
    const auto offset = checkout / "models/values/Offset.pipe";
    const auto bindings = build / "generated/bindings.json";
    const auto header = build / "generated/Model.hpp";
    invoke(compile);
    invoke({consumer.string(), "7"});
    const auto receipt = parseJson(readFile(bindings, 1U << 20));
    require(receipt.at("protocol") == "pipelang.native-model-artifacts.v2" &&
            receipt.at("source_set").at("models") == Json::array({"App", "Monitor"}),
            "installed source set lost its two explicit model exports");
    const auto manifest = parseJson(readFile(build / "generated/model.json", 1U << 20));
    require(manifest.at("protocol") == "pipelang.native-model-manifest.v2" &&
            manifest.at("classes").size() == 2 && manifest.at("objects").size() == 2 &&
            manifest.at("sourceInterfaces").size() == 1,
            "real multi-file object/interface/inheritance compilation was not admitted");
    const auto originalHeader = fileHash(header);
    const auto originalOffset = readFile(offset, 65536);
    auto editedOffset = originalOffset;
    const auto initial = editedOffset.find("amount = 3");
    require(initial != std::string::npos, "source dependency fixture marker is missing");
    editedOffset.replace(initial, std::string("amount = 3").size(), "amount = 7");
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(offset, editedOffset);
    invoke(compile);
    invoke({consumer.string(), "11"});
    require(fileHash(header) != originalHeader, "editing an imported dependency did not regenerate the model");
    const auto changedReceipt = readFile(bindings, 1U << 20);
    require(parseJson(changedReceipt).at("source_set") != receipt.at("source_set"),
            "dependency rebuild retained its old source receipt");

    const auto descriptorBytes = readFile(descriptor, 65536);
    const auto appBytes = readFile(app, 65536);
    auto expanded = parseJson(descriptorBytes);
    expanded["modules"].push_back({{"name", "Extras"}, {"path", "Extras.pipe"}});
    const auto extras = checkout / "models/Extras.pipe";
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(extras, "module Extras; public Record Marker { public int value = 5; }");
    auto importing = appBytes;
    importing.insert(importing.find('\n') + 1, "import Extras;\n");
    atomicWrite(app, importing);
    atomicWrite(descriptor, canonical(expanded));
    invoke(compile);
    invoke({consumer.string(), "11"});
    const auto expandedReceipt = parseJson(readFile(bindings, 1U << 20));
    require(expandedReceipt.at("source_set").at("modules").size() == 6,
            "descriptor addition did not reconfigure the closed dependency set");
    const auto extraHeader = fileHash(header);
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(extras, "module Extras; public Record Marker { public int value = 6; }");
    invoke(compile);
    require(fileHash(header) != extraHeader, "newly declared dependency was not tracked by CMake");

    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(app, appBytes);
    atomicWrite(descriptor, descriptorBytes);
    fs::remove(extras);
    invoke(compile);
    invoke({consumer.string(), "11"});
    require(readFile(bindings, 1U << 20) == changedReceipt,
            "descriptor removal retained an obsolete source dependency or generated artifact");

    const auto acceptedHeader = readFile(header, 1U << 20);
    const auto acceptedReceipt = readFile(bindings, 1U << 20);
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(extras, "module Extras; public Record Unreachable { }");
    atomicWrite(descriptor, canonical(expanded));
    const auto refused = process(compile, {}, std::chrono::seconds(90));
    require(refused.exit != 0, "compiler accepted an unreachable source-set module");
    require(readFile(header, 1U << 20) == acceptedHeader &&
            readFile(bindings, 1U << 20) == acceptedReceipt,
            "compiler refusal replaced the last good installed-consumer artifacts");
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(descriptor, descriptorBytes);
    fs::remove(extras);
    invoke(compile);
    invoke({consumer.string(), "11"});

    const auto sdkManifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto generator = nativeExecutable(prefix / "bin/pipelang-sdk-tool");
    const auto modelCompiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto viewCompiler = nativeExecutable(prefix / "bin/pipelang-model-viewc");
    const std::vector<std::string> base{generator.string(), "model-bindings",
        "--sdk-prefix", prefix.string(), "--sdk-manifest", sdkManifest.string(),
        "--sdk-manifest-sha256", fileHash(sdkManifest), "--model-compiler", modelCompiler.string(),
        "--source-set", descriptor.string(), "--namespace", "source_set_fixture",
        "--output-directory", (directory / "cli-refusal").string()};
    auto mixed = base;
    mixed.insert(mixed.end(), {"--model", app.string()});
    require(process(mixed).exit == 2, "SDK CLI accepted MODEL and SOURCE_SET together");
    auto view = base;
    view[1] = "bindings";
    view.insert(view.end(), {"--view-compiler", viewCompiler.string(), "--view", app.string()});
    require(process(view).exit == 2, "SDK view bindings accepted an unqualified source set");
}

void legacyUnsignedTokenBoundary(const fs::path& prefix) {
    const auto lexer = nativeExecutable(prefix / "bin/pipelang-cpp-lexer");
    const auto tokenize = [&](const std::string& source) {
        const auto result = process({lexer.string()}, canonical({{"contract", "pipelang.lexer.request.v1"}, {"source", source}}));
        require(result.exit == 0 && result.diagnostic.empty(),
                "legacy lexer transport failed for " + source + ": exit=" + std::to_string(result.exit) +
                "; stdout=" + result.output + "; stderr=" + result.diagnostic);
        return parseJson(result.output);
    };
    for (const std::string source : {"1u", "0xffffffffu"}) {
        const auto response = tokenize(source);
        require(response.at("failure").is_null() && response.at("tokens").size() == 3 &&
                response.at("tokens").at(0).at("kind") == "integer" &&
                response.at("tokens").at(1).at("kind") == "identifier",
                "legacy lexer acquired unsigned literal tokens");
    }
    for (const std::string source : {"&", "|", "^", "~", "%"}) {
        const auto response = tokenize(source);
        require(response.at("tokens").is_null() && response.at("failure").at("code") == "PL1001",
                "legacy lexer acquired unsigned operators");
    }
    for (const std::string source : {"<<", ">>"}) {
        const auto response = tokenize(source);
        require(response.at("failure").is_null() && response.at("tokens").size() == 3 &&
                response.at("tokens").at(0).at("literal") == source.substr(0, 1) &&
                response.at("tokens").at(1).at("literal") == source.substr(1, 1),
                "legacy lexer changed angle tokenization");
    }
}

void installedSdk(const fs::path& source, const fs::path& prefix, const fs::path& output) {
    legacyUnsignedTokenBoundary(prefix);

    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto hash = fileHash(manifest);
    const auto model = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto view = nativeExecutable(prefix / "bin/pipelang-model-viewc");
    const auto generated = output / "installed-bindings";
    generateBindings(prefix, manifest, hash, model, view,
        source / "tests/native-model-views/FixtureModel.pipe", source / "tests/native-model-views/Fixture.pview",
        "native_tooling", generated, true);
    require(fs::exists(generated / "Model.hpp") && fs::exists(generated / "ModelView.hpp") &&
            fs::exists(generated / "style.json"), "installed compiler outputs missing");
    const auto xmlGenerated = output / "installed-xml-bindings";
    generateBindings(prefix, manifest, hash, model, view,
        source / "tests/native-model-views/FixtureModel.pipe",
        source / "tests/native-model-views/Fixture.pxml", "native_tooling", xmlGenerated, true);
    require(readFile(xmlGenerated / "ModelView.hpp", 1U << 20) == readFile(generated / "ModelView.hpp", 1U << 20)
            && readFile(xmlGenerated / "view.json", 1U << 20) == readFile(generated / "view.json", 1U << 20)
            && readFile(xmlGenerated / "style.json", 1U << 20) == readFile(generated / "style.json", 1U << 20),
            "installed XML and declarative views generated different adapters, manifests or styles");
    const auto mislabeledXml = output / "XmlAsDeclarative.pview";
    const auto mislabeledDeclarative = output / "DeclarativeAsXml.pxml";
    atomicWrite(mislabeledXml, readFile(source / "tests/native-model-views/Fixture.pxml", 65536));
    atomicWrite(mislabeledDeclarative, readFile(source / "tests/native-model-views/Fixture.pview", 65536));
    const auto publishedAdapter = fileHash(generated / "ModelView.hpp");
    for (const auto& mislabeled : {mislabeledXml, mislabeledDeclarative}) {
        refuse([&] {
            generateBindings(prefix, manifest, hash, model, view,
                source / "tests/native-model-views/FixtureModel.pipe", mislabeled,
                "native_tooling", generated, true);
        }, "mismatched PipeView syntax");
        require(fileHash(generated / "ModelView.hpp") == publishedAdapter,
                "syntax refusal changed a previously published adapter");
    }
    const auto invalidView = output / "Fixture.vpipe";
    atomicWrite(invalidView, readFile(source / "tests/native-model-views/Fixture.pview", 65536));
    const auto completedView = fileHash(generated / "ModelView.hpp");
    refuse([&] {
        generateBindings(prefix, manifest, hash, model, view,
            source / "tests/native-model-views/FixtureModel.pipe", invalidView,
            "native_tooling", generated, true);
    }, "installed .vpipe view input", ".vpipe is not a PipeView source extension");
    require(fileHash(generated / "ModelView.hpp") == completedView,
            "installed rejected extension changed the compiled view");
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
            "-DVIEW_SOURCE=" + (source / "tests/native-model-views/Fixture.pview").string()});
    invoke({nativeTool("cmake").string(), "--build", (consumer / "build").string(), "--parallel", "1"});
    invoke({nativeExecutable(consumer / "build/consumer").string()});
    const auto xmlConsumer = consumer / "xml-build";
    invoke({nativeTool("cmake").string(), "-S", consumer.string(), "-B", xmlConsumer.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
            "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DMODEL_SOURCE=" + (source / "tests/native-model-views/FixtureModel.pipe").string(),
            "-DVIEW_SOURCE=" + (source / "tests/native-model-views/Fixture.pxml").string()});
    invoke({nativeTool("cmake").string(), "--build", xmlConsumer.string(), "--parallel", "1"});
    invoke({nativeExecutable(xmlConsumer / "consumer").string()});
    require(fileHash(xmlConsumer / "generated/ModelView.hpp") == fileHash(consumer / "build/generated/ModelView.hpp"),
            "installed CMake XML and declarative adapters differ");
    const auto rejectedConsumer = consumer / "rejected-extension";
    const auto rejectedConfiguration = process({nativeTool("cmake").string(), "-S", consumer.string(),
        "-B", rejectedConsumer.string(), "-DCMAKE_PREFIX_PATH=" + prefix.string(),
        "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
        "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
        "-DMODEL_SOURCE=" + (source / "tests/native-model-views/FixtureModel.pipe").string(),
        "-DVIEW_SOURCE=" + invalidView.string()}, {}, std::chrono::seconds(90));
    require(rejectedConfiguration.exit != 0 &&
            (rejectedConfiguration.output + rejectedConfiguration.diagnostic).find("VIEW requires a .pview or .pxml source") != std::string::npos,
            "installed CMake binding helper did not reject .vpipe at configuration");
    require(!fs::exists(rejectedConsumer / "generated/ModelView.hpp"),
            "refused installed CMake view input produced an adapter");
    const auto headless = output / "headless-consumer";
    invoke({nativeTool("cmake").string(), "-S", (source / "tests/sdk-headless-consumer").string(),
            "-B", headless.string(), "-DCMAKE_PREFIX_PATH=" + prefix.string(),
            "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE", "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", headless.string(), "--parallel", "1"});
    require(fs::exists(headless / "generated/Model.hpp") &&
            !fs::exists(headless / "generated/ModelView.hpp") && !fs::exists(headless / "generated/view.json"),
        "headless installed consumer generated UI artifacts");
    invoke({nativeExecutable(headless / "pipelang-headless-consumer").string()});
    invoke({nativeExecutable(headless / "pipelang-constants-leaf-consumer").string()});
    const auto headlessReceipt = parseJson(readFile(headless / "generated/bindings.json", 1U << 20));
    require(headlessReceipt.at("generated").at("StandaloneConstants.hpp") ==
            fileHash(headless / "generated/StandaloneConstants.hpp"),
            "standalone constant leaf is absent from the generated ownership inventory");
    const auto leaf = readFile(headless / "generated/StandaloneConstants.hpp", 1U << 20);
    require(leaf.find("pipelang/model") == std::string::npos && leaf.find("Qt") == std::string::npos,
            "standalone constants acquired runtime or Qt dependencies");
}
}

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        const auto name = fs::path(argv[0]).stem().string();
        if (name == "fake-model" || name == "fake-view")
            return fakeTool(name == "fake-model");
#ifndef _WIN32
        if (argc == 2 && std::string(argv[1]) == "--process-group") {
            std::cout << Json::array({::getpid(), ::getpgrp()}).dump();
            return 0;
        }
#endif
        if (argc == 2 && std::string(argv[1]) == "--static-method-contracts") {
            staticMethodManifestContracts();
            return 0;
        }
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
        if (argc == 5 && std::string(argv[1]) == "--linked-runtime-prepare") {
            const std::string mode = argv[4];
            require(mode == "normal" || mode == "sanitizer", "linked runtime mode must be normal or sanitizer");
            prepareLinkedRuntimeFixture(argv[2], argv[3], mode == "sanitizer");
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--compiled-owner-prepare") {
            const std::string mode = argv[4];
            require(mode == "normal" || mode == "sanitizer", "compiled owner mode must be normal or sanitizer");
            prepareCompiledOwnerFixture(argv[2], argv[3], mode == "sanitizer");
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--compiled-project-policy") {
            const std::string mode = argv[3];
            require(mode == "normal" || mode == "sanitizer", "compiled policy mode must be normal or sanitizer");
            verifyCompiledProjectPolicy(argv[2], mode == "sanitizer");
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--compiled-project-compare") {
            compareCompiledProjectBaseline(argv[2], argv[3], argv[4]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--compiled-project-fixture") {
            generateCompiledProjectFixture(argv[2], argv[3]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--compiled-project-contracts") {
            compiledProjectContracts(argv[2], argv[3]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--native-numeric-fixture") {
            nativeNumericExportContracts(argv[2], argv[3]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--source-library-contracts") {
            sourceLibraryContracts(argv[2], argv[3]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--project-fixture") {
            generateProjectFixture(argv[2], argv[3]);
            return 0;
        }
        require(argc == 4, "usage: sdk-tests SOURCE_ROOT|--project-fixture INSTALLED_SDK OUTPUT");
        const fs::path output = argv[3];
        require(!fs::exists(output), "test output must be fresh");
        fs::create_directories(output);
        const auto self = fs::canonical(argv[0]);
        staticMethodManifestContracts();
        primitiveContracts(output, self);
        admissionAndPublication(output, self);
        sourceSetAdmission(output, self);
        installedSdk(argv[1], argv[2], output);
        installedSourceSet(argv[1], argv[2], output);
        nativeNumericExportContracts(argv[2], output / "native-numeric");
        projectContracts(argv[2], output / "projects");
        projectModuleContracts(argv[2], output / "module-parts");
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"refusals", refusals},
            {"native_sdk_install_and_consume", true}, {"python_discovery_disabled", true}}));
        std::cout << refusals << " native SDK/tooling refusals; installed consumer passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
