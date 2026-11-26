// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ProjectTests.hpp"
#include "BuildTimestamp.h"
#include "ProjectFacetFixture.hpp"
#include "PureHostProjectFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include "../../src/native/sdk-tools/bindings.hpp"
#include "../../src/native/model-compiler/ScopedResourceBudget.hpp"

#include <iostream>

namespace {
using namespace pipelang::tools;

void expectRefusal(const std::function<void()>& operation, const std::string& expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                "project refusal did not explain " + expected + ": " + error.what());
        return;
    }
    throw std::runtime_error("project unexpectedly accepted " + expected);
}

Json compile(const fs::path& prefix, const Json& request) {
    const auto result = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                request.dump(), std::chrono::seconds(45), 4U << 20);
    require(result.exit == 0 && result.diagnostic.empty(), "project compiler transport failed: " + result.diagnostic);
    return parseJson(result.output, 4U << 20);
}

void invoke(const std::vector<std::string>& command) {
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "project consumer command failed: " + result.output + result.diagnostic);
}

void writeSources(const fs::path& root) {
    fs::create_directories(root / "src/domain");
    atomicWrite(root / "src/Entry.pipe", "import Domain.Counter; import Domain.Mode; import Domain.Host; import Point; import Facets.Group;\n");
    atomicWrite(root / "src/domain/Counter.pipe", R"pipe(module Domain.Counter;
import Domain.Item;
import Domain.Contract;
import Domain.Box;
public model Class Counter {
    public int value;
    private Readable item;
    public Counter(int initial) { value = initial; item = new Item(initial); }
    public pure int Read() => item.Read();
    public void Set(int next) { value = next; item = new Item(next); }
    public pure Box<int> Wrap(Box<int> input) => input;
}
)pipe");
    atomicWrite(root / "src/domain/Contract.pipe", R"pipe(module Domain.Contract;
public source Interface Readable { pure int Read(); }
)pipe");
    atomicWrite(root / "src/domain/Item.pipe", R"pipe(module Domain.Item;
import Domain.Contract;
public model Class Item : Readable {
    protected const int ProtectedOffset = 1;
    private int value;
    public Item(int initial) { value = initial; }
    public pure int Read() => value;
}
)pipe");
    atomicWrite(root / "src/domain/Box.pipe", "module Domain.Box; public Record Box<T> { public T Value; }\n");
    atomicWrite(root / "src/domain/Mode.pipe", "module Domain.Mode; public Enum Mode { Ready, Running }\n");
    atomicWrite(root / "src/domain/Host.pipe", "module Domain.Host; public Interface Store { Task<Result<int,string>> Read(); }\n");
    atomicWrite(root / "src/domain/Point.pipe", "public Record Point { public int value; }\n");
    // Indexing permits large unrelated domains without adding their declarations
    // or bytes to this entry's unchanged 64 KiB compilation budget.
    atomicWrite(root / "src/Unrelated.pipe", "module Unrelated; /*" + std::string(40000, 'x') + "*/ public Record Detached {}\n");
    atomicWrite(root / "src/Another.pipe", "/*" + std::string(40000, 'x') + "*/ public Record Detached {}\n");
    // View languages are separate inputs, never ordinary model modules.
    atomicWrite(root / "src/Screen.pview", "view Screen(model: Counter) { Label(id: \"screen\", text: \"View\"); }\n");
    atomicWrite(root / "src/Screen.pxml", "<View name=\"Screen\"/>\n");
    atomicWrite(root / "src/metadata.xml", "<metadata/>\n");
    writeProjectFacetSources(root);
}

void admission(const fs::path& prefix, const fs::path& root) {
    const auto project = SourceProject::load(root, "src", "Entry.pipe");
    const auto request = project.request("project_fixture");
    require(request.at("modules").size() == 12, "project did not select the exact transitive import closure");
    require(project.dependencies().size() == 14, "project did not retain its complete discovery inventory");
    for (const auto& dependency : project.dependencies())
        require(dependency.extension() == ".pipe", "ordinary model discovery admitted a view or unrelated XML input");
    for (const auto& entry : {"Screen.pview", "Screen.pxml", "metadata.xml"})
        expectRefusal([&] { SourceProject::load(root, "src", entry); }, "project entry must be a .pipe source file");
    require(request.at("entry") == "Entry", "module-free entry did not use its filename identity");
    for (const auto& module : request.at("modules")) {
        if (module.at("name") == "Point")
            require(module.at("source") == readFile(root / "src/domain/Point.pipe", 65536),
                    "implicit module discovery rewrote authored source bytes");
    }
    const auto response = compile(prefix, request);
    require(response.at("ok") == true, "project compilation failed: " + response.dump());
    const auto generated = response.at("header").get<std::string>();
    const auto types = response.at("types_header").get<std::string>();
    require(generated.find("#include \"Types.hpp\"") != std::string::npos &&
            types.find("pl_object_function_") == std::string::npos &&
            types.find("struct pl_object_state_") == std::string::npos &&
            types.find("GraphModelTransaction<") == std::string::npos,
            "project type header contains execution implementations");
    require(generated.find("template<class pl_Execution>\nrequires std::is_same_v<pl_Execution, pipelang::model::ObjectExecution>")
                != std::string::npos && generated.find("pl_execution.transaction.template read<") != std::string::npos,
            "project private object bodies lost dependent execution lowering");
    require(response.at("manifest").at("classes").size() == 4 &&
            response.at("manifest").at("objects").size() == 5,
            "source model modifier did not preserve ordinary object ownership");
    const auto model = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    expectRefusal([&] {
        generateProjectBindings(prefix, manifest, fileHash(manifest), model, root, "src", "Entry.pipe",
                                "project_fixture", root / "src");
    }, "unowned material");
    expectRefusal([&] { SourceProject::load(root, "../src", "Entry.pipe"); }, "path");
    expectRefusal([&] { SourceProject::load(root, "src", "../Entry.pipe"); }, "path");
    atomicWrite(root / "src/Duplicate.pipe", "module Domain.Item; public Record Duplicate {}\n");
    expectRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    fs::remove(root / "src/Duplicate.pipe");
    atomicWrite(root / "src/ExplicitCollision.pipe", "module Point; public Record Different {}\n");
    expectRefusal([&] { project.verifyUnchanged(); }, "duplicate project module 'Point'");
    expectRefusal([&] { project.verifyUnchanged(); }, "ExplicitCollision.pipe");
    expectRefusal([&] { project.verifyUnchanged(); }, "domain/Point.pipe");
    fs::remove(root / "src/ExplicitCollision.pipe");
    atomicWrite(root / "src/duplicates/Point.pipe", "public Record Different {}\n");
    expectRefusal([&] { project.verifyUnchanged(); }, "duplicates/Point.pipe");
    fs::remove(root / "src/duplicates/Point.pipe");
    fs::remove(root / "src/duplicates");
    for (const auto& invalid : {"bad-name", "bad.name", "class", "base", "namespace", "concept", "pl_reserved", "two words", " leading"}) {
        const auto path = root / "src" / (std::string(invalid) + ".pipe");
        atomicWrite(path, "public Record Data {}\n");
        expectRefusal([&] { SourceProject::load(root, "src", "Entry.pipe"); }, "declare an explicit module");
        atomicWrite(path, "module ExplicitName; public Record Data {}\n");
        const auto admitted = SourceProject::load(root, "src", "Entry.pipe");
        require(admitted.dependencies().size() == project.dependencies().size() + 1,
                "explicit module declaration did not override invalid filename stem");
        fs::remove(path);
    }
    atomicWrite(root / "src/Added.pipe", "module Added; public Record Added {}\n");
    expectRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    fs::remove(root / "src/Added.pipe");
    const auto entry = root / "src/Entry.pipe";
    const auto original = readFile(entry, 65536);
    atomicWrite(entry, original + "import Missing;\n");
    expectRefusal([&] { SourceProject::load(root, "src", "Entry.pipe"); }, "missing imported");
    atomicWrite(entry, original + "import Entry;\n");
    expectRefusal([&] { SourceProject::load(root, "src", "Entry.pipe"); }, "cycle");
    atomicWrite(entry, original + "import Domain.Counter;\n");
    expectRefusal([&] { SourceProject::load(root, "src", "Entry.pipe"); }, "duplicate project import");
    atomicWrite(entry, original + "import Unrelated; import Another;\n");
    require(SourceProject::load(root, "src", "Entry.pipe").request("large_project").at("modules").size() > 2,
            "project retained the obsolete 64KiB source closure ceiling");
    atomicWrite(entry, original);
    fs::rename(root / "src/Another.pipe", root / "Another.saved");
    expectRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    fs::rename(root / "Another.saved", root / "src/Another.pipe");
#ifndef _WIN32
    fs::create_directory_symlink(root / "src/domain", root / "src/redirect");
    expectRefusal([&] { SourceProject::load(root, "src", "Entry.pipe"); }, "symbolic");
    fs::remove(root / "src/redirect");
#endif
    project.verifyUnchanged();
    auto library = request;
    library["entry"] = "Library";
    library["modules"] = Json::array({{{"name", "Library"}, {"source", "module Library; public Record Data {} public Class Item {}"}}});
    const auto libraryResponse = compile(prefix, library);
    require(libraryResponse.at("ok") == true && libraryResponse.at("manifest").at("classes").empty(),
            "class library incorrectly requires a model root");
    auto implicitLibrary = library;
    implicitLibrary["modules"][0]["source"] = "public Record Data {} public Class Item {}";
    const auto implicitLibraryResponse = compile(prefix, implicitLibrary);
    require(implicitLibraryResponse.at("ok") == true &&
            implicitLibraryResponse.at("manifest") == libraryResponse.at("manifest"),
            "implicit module identity changed public source identities");
    auto oldModules = implicitLibrary;
    oldModules["protocol"] = "pipelang.native-model.v2";
    require(compile(prefix, oldModules).at("ok") == false, "implicit modules leaked into v2");
    oldModules["protocol"] = "pipelang.native-model.v3";
    oldModules["models"] = Json::array({"Library.Item"});
    require(compile(prefix, oldModules).at("ok") == false, "implicit modules leaked into v3");
    auto collision = library;
    collision["modules"] = Json::array({
        {{"name", "Library"}, {"source", "module Library; import Library.Data; public Record Data {}"}},
        {{"name", "Library.Data"}, {"source", "module Library.Data; public Record Value {}"}}});
    require(compile(prefix, collision).at("ok") == false, "native module/type namespace collision was accepted");
    for (const auto& reserved : {"namespace", "concept", "requires", "Outer.co_await"}) {
        auto implicitReserved = library;
        implicitReserved["entry"] = reserved;
        implicitReserved["modules"] = Json::array({{{"name", reserved}, {"path", "Reserved.pipe"},
                                                   {"source", "public Record Value {}"}}});
        const auto refusal = compile(prefix, implicitReserved);
        require(refusal.at("ok") == false && refusal.at("diagnostics").at(0).at("path") == "Reserved.pipe",
                "reserved implicit module emitted invalid C++ or lost its origin");
    }
    for (const auto& source : {
        "module namespace; public Record Value {}",
        "module Library; public Class concept {}",
        "module Library; public Record requires {}"}) {
        auto explicitReserved = library;
        explicitReserved["modules"][0]["path"] = "Reserved.pipe";
        explicitReserved["modules"][0]["source"] = source;
        const auto refusal = compile(prefix, explicitReserved);
        require(refusal.at("ok") == false && refusal.at("diagnostics").at(0).at("path") == "Reserved.pipe",
                "owning identifier policy admitted an invalid native export name");
    }
    auto interfaceRoot = library;
    interfaceRoot["modules"][0]["source"] = "module Library; public source Interface I {} public model Class Root : I {}";
    require(compile(prefix, interfaceRoot).at("ok") == true,
            "native owner marker disabled the source interface facet");
    for (const auto& source : {
        "module Library; public Class Base {} public model Class Root : Base {}",
        "module Library; public model Class Root { public pure Root Self() => this; }",
        "module Library; public model Class Root { private Root() {} }",
        "module Library; public model Class Root { public int Reference; }"}) {
        auto unsupportedRoot = library;
        unsupportedRoot["modules"][0]["source"] = source;
        const auto refusal = compile(prefix, unsupportedRoot);
        require(refusal.at("ok") == false && refusal.at("diagnostics").dump().find("native owner facet") != std::string::npos,
                "unsupported native root shape lacks an explicit facet diagnostic");
    }
    auto recursiveRoot = library;
    recursiveRoot["modules"][0]["source"] = R"pipe(module Library;
public model Class Root {
    public Root(bool nested) {
        if (nested) {
            Root child = new Root(false);
        }
    }
}
)pipe";
    const auto recursiveAdmission = compile(prefix, recursiveRoot);
    require(recursiveAdmission.at("ok") == true,
            "source object facet rejected runtime-bounded constructor recursion");
    auto privateAccess = library;
    privateAccess["modules"][0]["source"] = "module Library; public model Class Root { private int secret; } public model Class Other { private Root item = new Root(); public pure int Read() => item.secret; }";
    const auto privateRefusal = compile(prefix, privateAccess);
    require(privateRefusal.at("ok") == false && privateRefusal.at("diagnostics").dump().find("private") != std::string::npos,
            "facet access identity exposed private members to another source class");
    auto legacyRoot = library;
    legacyRoot["protocol"] = "pipelang.native-model.v3";
    legacyRoot["models"] = Json::array({"Library.Root"});
    legacyRoot["modules"][0]["source"] = "module Library; public Class Root {} public Class Other { private Root item = new Root(); }";
    require(compile(prefix, legacyRoot).at("ok") == false,
            "project dual facets changed frozen v3 selected-root semantics");
    auto directRoot = library;
    directRoot["modules"][0]["source"] = "module Library; public model Class Root { public int value; public Root(int initial) { value = initial; } public pure int Read() => value; public void Set(int next) { value = next; } }";
    const auto projectRoot = compile(prefix, directRoot);
    require(projectRoot.at("ok") == true, "direct project native root failed");
    directRoot["protocol"] = "pipelang.native-model.v3";
    directRoot["models"] = Json::array({"Library.Root"});
    directRoot["modules"][0]["source"] = "module Library; public Class Root { public int value; public Root(int initial) { value = initial; } public pure int Read() => value; public void Set(int next) { value = next; } }";
    const auto selectedRoot = compile(prefix, directRoot);
    require(selectedRoot.at("ok") == true && !selectedRoot.contains("types_header"), "legacy native-root comparison failed");
    const auto rootBody = [](std::string header, const std::string& nativeName) {
        const auto begin = header.find("class " + nativeName + " {\n");
        const auto end = header.find("\n};\n", begin);
        require(begin != std::string::npos && end != std::string::npos, "native root body was not emitted");
        auto body = header.substr(begin, end + 4 - begin);
        const auto reference = body.find("    using Reference = ");
        if (reference != std::string::npos)
            body.erase(reference, body.find('\n', reference) + 1 - reference);
        std::size_t position = 0;
        while ((position = body.find(nativeName, position)) != std::string::npos) {
            body.replace(position, nativeName.size(), "Root");
            position += 4;
        }
        return body;
    };
    require(rootBody(projectRoot.at("header").get<std::string>(),
                     projectRoot.at("manifest").at("classes").at(0).at("name").get<std::string>()) ==
            rootBody(selectedRoot.at("header").get<std::string>(),
                     selectedRoot.at("manifest").at("classes").at(0).at("name").get<std::string>()),
            "project object facet changed direct native state, lifetime or invocation lowering");
    // Native project arithmetic shares sequenced binary32 helpers; the frozen
    // v3 route retains its original materialized closure lowering.
    const std::string arithmeticClass = "Class Root { public pure float32 Arithmetic(float32 a, float32 b, float32 c) => a * b + c; }";
    auto projectArithmetic = library;
    projectArithmetic["modules"][0]["source"] = "module Library; public model " + arithmeticClass;
    auto legacyArithmetic = directRoot;
    legacyArithmetic["modules"][0]["source"] = "module Library; public " + arithmeticClass;
    const auto projectArithmeticResult = compile(prefix, projectArithmetic);
    const auto legacyArithmeticResult = compile(prefix, legacyArithmetic);
    require(projectArithmeticResult.at("ok") == true && legacyArithmeticResult.at("ok") == true,
            "native arithmetic comparison did not compile");
    const auto projectArithmeticBody = rootBody(projectArithmeticResult.at("header").get<std::string>(),
        projectArithmeticResult.at("manifest").at("classes").at(0).at("name").get<std::string>());
    const auto legacyArithmeticBody = rootBody(legacyArithmeticResult.at("header").get<std::string>(),
        legacyArithmeticResult.at("manifest").at("classes").at(0).at("name").get<std::string>());
    require(projectArithmeticBody.find("addBinary32(pipelang::model::Binary32Operands{") != std::string::npos &&
            projectArithmeticBody.find("multiplyBinary32(pipelang::model::Binary32Operands{") != std::string::npos,
            "native project arithmetic did not use sequenced binary32 lowering");
    require(legacyArithmeticBody.find("Binary32Operands") == std::string::npos &&
            legacyArithmeticBody.find("volatile float pl_result") != std::string::npos,
            "native project arithmetic changed frozen v3 lowering");
    // Closed generic methods fit the legacy specialization budget, while
    // cloning their normalized bodies must consume the same remaining budget.
    std::string budgetSource = "module Library;\n";
    for (int index = 0; index < 60; ++index)
        budgetSource += "public Enum E" + std::to_string(index) + " { Value }\n";
    const auto rootModifier = budgetSource.size();
    budgetSource += "public model Class Root { private const int Seed = 1; ";
    budgetSource += "private pure int Work<T>(int input) { int total = Seed; ";
    for (int index = 0; index < 128; ++index)
        budgetSource += "total = total + input; ";
    budgetSource += "return total; } public pure int Run() => ";
    for (int index = 0; index < 60; ++index) {
        if (index != 0)
            budgetSource += " + ";
        budgetSource += "Work<E" + std::to_string(index) + ">(1)";
    }
    budgetSource += "; }";
    auto cloneBudget = library;
    cloneBudget["modules"][0]["source"] = budgetSource;
    const auto cloneRefusal = compile(prefix, cloneBudget);
    require(cloneRefusal.at("ok") == false &&
            cloneRefusal.at("diagnostics").dump().find("AST/type node budget exceeded") != std::string::npos,
            "normalized project facet copies escaped the cumulative clone budget");
    budgetSource.erase(rootModifier + std::string("public ").size(), std::string("model ").size());
    cloneBudget["protocol"] = "pipelang.native-model.v3";
    cloneBudget["models"] = Json::array({"Library.Root"});
    cloneBudget["modules"][0]["source"] = budgetSource;
    require(compile(prefix, cloneBudget).at("ok") == true,
            "project clone accounting changed legacy specialization admission");
    const Json legacy{{"protocol", "pipelang.native-model.v1"}, {"source", "public Class model {}"}};
    require(compile(prefix, legacy).at("ok") == true, "project modifier reserved a legacy identifier");
    auto legacyModifier = legacy;
    legacyModifier["source"] = "public model Class Root {}";
    require(compile(prefix, legacyModifier).at("ok") == false, "project modifier changed old protocol grammar");
}

void sourceLibraryAdmission(const fs::path& prefix, const fs::path& root) {
    const auto projectRoot = root / "application";
    const auto libraryRoot = root / "library";
    const auto referencesPath = root / "libraries.json";
    const auto descriptorPath = libraryRoot / "library.json";
    fs::create_directories(projectRoot / "src");
    fs::create_directories(libraryRoot / "src");
    atomicWrite(projectRoot / "src/Entry.pipe",
                "module App; import Library.Contracts; public model Class Root { private First value; public Root() { value = new First(); } public pure int Value() => value.Read(); }\n");
    atomicWrite(libraryRoot / "src/First.pipe", "module Library.Contracts; public Class First { public First() {} public pure int Read() => 7; }\n");
    atomicWrite(libraryRoot / "src/Second.pipe", "module Library.Contracts; public Record Second {}\n");
    Json descriptor{{"protocol", "pipelang.source-library.v1"}, {"name", "Library.Contracts"},
                    {"version", "0.1.0"}, {"source_root", "src"}, {"sources", Json::array()}};
    for (const auto& name : {"First.pipe", "Second.pipe"})
        descriptor["sources"].push_back({{"path", name}, {"sha256", fileHash(libraryRoot / "src" / name)}});
    Json references{{"protocol", "pipelang.source-library-references.v1"}, {"libraries", Json::array()}};
    const auto pinDescriptor = [&] {
        atomicWrite(descriptorPath, canonical(descriptor));
        references["libraries"] = Json::array({{{"root", libraryRoot.generic_string()},
            {"descriptor", "library.json"}, {"sha256", fileHash(descriptorPath)}}});
        atomicWrite(referencesPath, canonical(references));
    };
    const auto load = [&] { return SourceProject::load(projectRoot, "src", "Entry.pipe", referencesPath); };
    pinDescriptor();
    const auto originalDescriptor = descriptor;
    const auto originalReferences = references;
    const auto project = load();
    require(project.request("source_library").at("modules").size() == 3,
            "source libraries did not contribute all selected explicit module parts");
    require(project.dependencies().size() == 5 && project.sourceDirectories().size() == 2,
            "source library metadata or source dependencies were omitted");
    require(project.receipt().at("source_libraries")[0].at("version") == "0.1.0",
            "source library receipt omitted version identity");
    require(project.receipt().at("closure_sources") == Json::array({"project/Entry.pipe",
                "libraries/Library.Contracts/First.pipe", "libraries/Library.Contracts/Second.pipe"}),
            "source library diagnostics lost deterministic source ownership");
    require(compile(prefix, project.request("source_library")).at("ok") == true,
            "closed project compiler transport refused source library modules");
    project.verifyUnchanged();
    auto limitedBudget = pipelang::models::resourceBudget();
    limitedBudget.indexedFiles = 2;
    {
        const pipelang::models::ScopedResourceBudget limit(limitedBudget);
        expectRefusal(load, "indexed_files resource budget exceeded");
    }
    limitedBudget = pipelang::models::resourceBudget();
    limitedBudget.directoryEntries = 2;
    {
        const pipelang::models::ScopedResourceBudget limit(limitedBudget);
        expectRefusal(load, "directory_entries resource budget exceeded");
    }

    atomicWrite(libraryRoot / "src/Extra.pipe", "module Unselected; public Record Extra {}\n");
    expectRefusal(load, "pinned inventory membership mismatch");
    expectRefusal([&] { project.verifyUnchanged(); }, "pinned inventory membership mismatch");
    fs::remove(libraryRoot / "src/Extra.pipe");
    const auto sourcePath = libraryRoot / "src/First.pipe";
    const auto originalSource = readFile(sourcePath, 65536);
    atomicWrite(sourcePath, originalSource + "// drift\n");
    expectRefusal(load, "source SHA256 mismatch");
    expectRefusal([&] { project.verifyUnchanged(); }, "source SHA256 mismatch");
    atomicWrite(sourcePath, originalSource);
    fs::rename(sourcePath, libraryRoot / "First.saved");
    expectRefusal(load, "pinned inventory membership mismatch");
    fs::rename(libraryRoot / "First.saved", sourcePath);
    atomicWrite(descriptorPath, canonical(descriptor) + "\n");
    expectRefusal(load, "descriptor SHA256 mismatch");
    pinDescriptor();
    atomicWrite(referencesPath, canonical(references) + "\n");
    expectRefusal([&] { project.verifyUnchanged(); }, "changed during compilation");
    atomicWrite(referencesPath, canonical(references));

    references["libraries"][0]["root"] = "relative/library";
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "absolute and normalized");
    references = originalReferences;
    references["libraries"][0]["descriptor"] = "../library.json";
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "path escapes");
    references = originalReferences;
    descriptor["source_root"] = "../src";
    pinDescriptor();
    expectRefusal(load, "path escapes");
    descriptor = originalDescriptor;
    descriptor["sources"][0]["path"] = "../First.pipe";
    pinDescriptor();
    expectRefusal(load, "path escapes");
    descriptor = originalDescriptor;
    descriptor["sources"][1]["path"] = "first.pipe";
    pinDescriptor();
    expectRefusal(load, "case-folded source path collision");
    descriptor = originalDescriptor;
    std::swap(descriptor["sources"][0], descriptor["sources"][1]);
    pinDescriptor();
    expectRefusal(load, "unique sorted paths");
    descriptor = originalDescriptor;
    descriptor["unexpected"] = true;
    pinDescriptor();
    expectRefusal(load, "unexpected field");
    descriptor = originalDescriptor;
    pinDescriptor();
    references["libraries"].push_back(references["libraries"][0]);
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "duplicate source library name");
    pinDescriptor();

    atomicWrite(sourcePath, "module App; public Record Conflicting {}\n");
    descriptor["sources"][0]["sha256"] = fileHash(sourcePath);
    pinDescriptor();
    expectRefusal(load, "module ownership collision: App");
    atomicWrite(sourcePath, originalSource);
    descriptor = originalDescriptor;
    pinDescriptor();
    const auto secondRoot = root / "second-library";
    fs::create_directories(secondRoot / "src");
    atomicWrite(secondRoot / "src/Part.pipe", "module Library.Contracts; public Record Third {}\n");
    Json secondDescriptor{{"protocol", "pipelang.source-library.v1"}, {"name", "Another.Library"},
        {"version", "0.2.0"}, {"source_root", "src"}, {"sources", Json::array({
            {{"path", "Part.pipe"}, {"sha256", fileHash(secondRoot / "src/Part.pipe")}}})}};
    atomicWrite(secondRoot / "library.json", canonical(secondDescriptor));
    references["libraries"].push_back({{"root", secondRoot.generic_string()}, {"descriptor", "library.json"},
                                     {"sha256", fileHash(secondRoot / "library.json")}});
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "module ownership collision: Library.Contracts");
    atomicWrite(secondRoot / "src/Part.pipe", "module Additional.Contracts; public Record Third {}\n");
    secondDescriptor["sources"][0]["sha256"] = fileHash(secondRoot / "src/Part.pipe");
    atomicWrite(secondRoot / "library.json", canonical(secondDescriptor));
    references["libraries"][1]["sha256"] = fileHash(secondRoot / "library.json");
    atomicWrite(referencesPath, canonical(references));
    const auto orderedRequest = load().request("source_library");
    std::swap(references["libraries"][0], references["libraries"][1]);
    atomicWrite(referencesPath, canonical(references));
    require(load().request("source_library") == orderedRequest, "library reference order changed compiler inputs");
    std::swap(references["libraries"][0], references["libraries"][1]);
    secondDescriptor = originalDescriptor;
    secondDescriptor["name"] = "Another.Library";
    atomicWrite(libraryRoot / "another.json", canonical(secondDescriptor));
    references["libraries"][1] = {{"root", libraryRoot.generic_string()}, {"descriptor", "another.json"},
                                 {"sha256", fileHash(libraryRoot / "another.json")}};
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "roots must not overlap");
    pinDescriptor();
#ifndef _WIN32
    fs::create_symlink(sourcePath, libraryRoot / "src/Linked.pipe");
    expectRefusal(load, "symbolic or reparse");
    fs::remove(libraryRoot / "src/Linked.pipe");
    fs::create_directory_symlink(libraryRoot, root / "linked-library");
    references["libraries"][0]["root"] = (root / "linked-library").generic_string();
    atomicWrite(referencesPath, canonical(references));
    expectRefusal(load, "symbolic or reparse");
    fs::remove(root / "linked-library");
    pinDescriptor();
#endif
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto sdkHash = fileHash(manifest);
    const auto information = projectInformation(prefix, manifest, sdkHash, projectRoot, "src", "Entry.pipe", referencesPath);
    require(information.at("source_directories").size() == 2,
            "project information omitted library membership watches");
    const auto generated = generateProjectBindings(prefix, manifest, sdkHash, compiler,
        projectRoot, "src", "Entry.pipe", "source_library", root / "generated", {}, false, false, referencesPath);
    require(generated.at("project") == project.receipt(), "binding receipt lost source library pins");
    expectRefusal([&] {
        generateProjectBindings(prefix, manifest, sdkHash, compiler, projectRoot, "src", "Entry.pipe",
            "source_library", libraryRoot / "src/generated", {}, false, false, referencesPath);
    }, "outputs must be outside");
    const auto plan = compiledProjectInformation(prefix, manifest, sdkHash, compiler,
        projectRoot, "src", "Entry.pipe", "source_library_compiled", {}, false, referencesPath);
    const auto compiled = generateCompiledProjectBindings(prefix, manifest, sdkHash, compiler,
        projectRoot, "src", "Entry.pipe", "source_library_compiled", root / "compiled",
        plan.at("plan_sha256").get<std::string>(), {}, false, false, referencesPath);
    require(compiled.at("project") == project.receipt(), "compiled binding receipt lost source library pins");
}

void writeConsumer(const fs::path& root, bool compiled = false) {
    writeProjectFacetConsumer(root);
    std::string buildDescription = R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(ProjectConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(project_model PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Entry.pipe
    NAMESPACE project_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE project_model)
add_executable(types_consumer types-consumer.cpp)
target_link_libraries(types_consumer PRIVATE project_model)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(consumer PRIVATE -O3 -ffp-contract=fast)
endif()
)cmake";
    if (compiled) {
        const std::string selection = "pipelang_add_model(project_model ";
        const auto position = buildDescription.find(selection);
        require(position != std::string::npos, "compiled fixture lost its model declaration");
        buildDescription.insert(position + selection.size(), "COMPILATION COMPILED BATCH_IMPLEMENTATIONS ");
        buildDescription += R"cmake(
add_executable(compiled_peer compiled-main.cpp compiled-peer.cpp)
target_link_libraries(compiled_peer PRIVATE project_model)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(TARGET project_model_implementation consumer types_consumer compiled_peer
    PROPERTY EXPORT_COMPILE_COMMANDS ON)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/implementation-sources.txt"
    CONTENT "$<JOIN:$<FILTER:$<TARGET_PROPERTY:project_model_implementation,SOURCES>,INCLUDE,Implementation_[0-9]+[.]cpp$>,\n>\n"
    NEWLINE_STYLE UNIX)
add_library(compiled_policy INTERFACE)
target_compile_definitions(compiled_policy INTERFACE PIPELANG_COMPILED_TRANSITIVE_DEFINITION=19)
target_compile_options(compiled_policy INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-DPIPELANG_COMPILED_TRANSITIVE_OPTION=31>")
target_link_libraries(project_model INTERFACE compiled_policy)
target_compile_definitions(project_model INTERFACE "$<$<CONFIG:Release>:PIPELANG_COMPILED_DEFINITION=17>")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(project_model INTERFACE -ffp-contract=fast -DPIPELANG_COMPILED_OPTION=23
        "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/compiled-policy-check.hpp\"")
endif()
option(PIPELANG_COMPILED_FIXTURE_SANITIZERS "Instrument every generated implementation and caller" OFF)
if(PIPELANG_COMPILED_FIXTURE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Compiled fixture sanitizer flags require GNU or Clang")
    endif()
    target_compile_options(project_model INTERFACE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_model INTERFACE -fsanitize=address,undefined)
endif()
enable_testing()
add_test(NAME compiled-facets COMMAND consumer)
add_test(NAME compiled-types COMMAND types_consumer)
add_test(NAME compiled-multi-tu COMMAND compiled_peer)
)cmake";
    }
    atomicWrite(root / "CMakeLists.txt", buildDescription);
    atomicWrite(root / "types-consumer.cpp", R"cpp(#include "Types.hpp"
#include <cstdint>
#include <type_traits>
using project_fixture::source::Domain::Box::Box;
using project_fixture::source::Domain::Contract::Readable;
using project_fixture::source::Domain::Host::Store;
using project_fixture::source::Domain::Mode::Mode;
using project_fixture::source::Point::Point;
static_assert(std::is_abstract_v<Store>);
int main() {
    const Box<std::int64_t> value{17};
    const Point point{23};
    const Readable emptyReference;
    (void)emptyReference;
    return value.Value == 17 && point.value == 23 && Mode::Ready != Mode::Running ? 0 : 1;
}
)cpp");
    atomicWrite(root / "main.cpp", R"cpp(#include "Model.hpp"
#include <cstdint>
#include <type_traits>
#include "facet-consumer.hpp"

using project_fixture::source::Domain::Counter::Counter;
using project_fixture::source::Domain::Box::Box;
using project_fixture::source::Domain::Item::Item;
using project_fixture::source::Domain::Contract::Readable;
using project_fixture::source::Domain::Host::Store;
using project_fixture::source::Domain::Mode::Mode;
using project_fixture::source::Point::Point;
static_assert(std::is_abstract_v<Store>);
static_assert(!std::is_same_v<Item::Reference, Readable>);
int main() {
    facet_test::verify();
    Counter counter(7);
    Box<std::int64_t> value{11};
    const auto wrapped = counter.Wrap(value);
    const auto read = counter.Read();
    const auto initial = counter.snapshot();
    counter.Set(13);
    const auto updated = counter.Read();
    const auto mode = Mode::Ready;
    const Point point{23};
    return initial.value == 7 && counter.snapshot().value == 13 && updated == 13 &&
           wrapped.Value == 11 && read == 7 && mode == Mode::Ready && point.value == 23 ? 0 : 1;
}
)cpp");
}

void installedConsumer(const fs::path& prefix, const fs::path& root) {
    writeConsumer(root);
    const auto build = root / "build";
    invoke({nativeTool("cmake").string(), "-S", root.string(), "-B", build.string(),
            "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
            "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
            "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"});
    invoke({nativeExecutable(build / "types_consumer").string()});
    invoke({nativeExecutable(build / "consumer").string()});
    const auto typesPath = build / "generated/Types.hpp";
    const auto typesHash = fileHash(typesPath);
    pipelang::tests::waitForBuildTimestampTick(build);
    fs::remove(typesPath);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "project_model_generate", "--parallel", "1"});
    require(fileHash(typesPath) == typesHash, "project Types.hpp was not regenerated as a declared output");
    const auto receiptPath = build / "generated/bindings.json";
    const auto published = parseJson(readFile(receiptPath, 4U << 20));
    require(published.at("generated").at("Types.hpp") == fileHash(typesPath),
            "project type header was not included in the publication receipt");
    auto previous = fileHash(receiptPath);
    const auto regenerate = [&] {
        invoke({nativeTool("cmake").string(), "--build", build.string(), "--target", "project_model_generate", "--parallel", "1"});
        const auto current = fileHash(receiptPath);
        require(current != previous, "project mutation did not trigger incremental generation");
        previous = current;
    };
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(root / "src/New.pipe", "public Record Added {}\n");
    regenerate();
    const auto entry = root / "src/Entry.pipe";
    const auto original = readFile(entry, 65536);
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(entry, original + "import New;\n");
    regenerate();
    auto receipt = parseJson(readFile(receiptPath, 1U << 20));
    require(receipt.at("project").at("closure").size() == 13, "new import did not change the compiler closure");
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(root / "src/New.pipe", "public Record Added { public int value; }\n");
    regenerate();
    pipelang::tests::waitForBuildTimestampTick(build);
    atomicWrite(entry, original);
    regenerate();
    pipelang::tests::waitForBuildTimestampTick(build);
    fs::remove(root / "src/New.pipe");
    regenerate();
}
}

void sourceLibraryContracts(const std::filesystem::path& prefix, const std::filesystem::path& output) {
    require(!fs::exists(output), "source library test output must be fresh");
    fs::create_directories(output);
    sourceLibraryAdmission(prefix, output);
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"pinned_source_libraries", true}, {"closed_compiler_transport", true},
        {"inventory_hash_path_and_ownership_refusals", true},
        {"header_and_compiled_publication", true}}));
}

void generateProjectFixture(const std::filesystem::path& prefix, const std::filesystem::path& output) {
    require(!fs::exists(output), "project fixture output must be fresh");
    fs::create_directories(output);
    writeSources(output);
    writeConsumer(output);
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    generateProjectBindings(prefix, manifest, fileHash(manifest),
                            nativeExecutable(prefix / "bin/pipelang-modelc"), output, "src", "Entry.pipe",
                            "project_fixture", output / "generated");
}

void projectContracts(const std::filesystem::path& prefix, const std::filesystem::path& output) {
    require(!fs::exists(output), "project test output must be fresh");
    fs::create_directories(output);
    writeSources(output);
    admission(prefix, output);
    sourceLibraryAdmission(prefix, output / "source-libraries");
    installedConsumer(prefix, output);
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"project_discovery_and_import_closure", true}, {"stable_native_source_names", true},
        {"incremental_add_remove_modify_import", true}, {"source_drift_and_refusals", true}}));
    std::cout << "project discovery, native aliases and incremental consumer passed\n";
}

void generateCompiledProjectFixture(const std::filesystem::path& prefix,
                                    const std::filesystem::path& output) {
    require(!fs::exists(output), "compiled project fixture output must be fresh");
    fs::create_directories(output);
    writeSources(output);
    writeConsumer(output, true);
    writePureHostProjectFixture(output / "pure-host-values");
    atomicWrite(output / "compiled-policy-check.hpp", R"cpp(#pragma once
#if PIPELANG_COMPILED_DEFINITION != 17 || PIPELANG_COMPILED_OPTION != 23
#error Direct producer policy was not applied
#endif
#if PIPELANG_COMPILED_TRANSITIVE_DEFINITION != 19 || PIPELANG_COMPILED_TRANSITIVE_OPTION != 31
#error Transitive producer policy was not applied
#endif
)cpp");
    atomicWrite(output / "compiled-peer.cpp", R"cpp(#include "Model.hpp"
using project_fixture::source::Domain::Counter::Counter;
std::int64_t readCompiledPeer(const Counter& value) {
    return value.Read();
}
)cpp");
    atomicWrite(output / "compiled-main.cpp", R"cpp(#include "Model.hpp"
#include <type_traits>
using project_fixture::source::Domain::Counter::Counter;
std::int64_t readCompiledPeer(const Counter&);
static_assert(!std::is_copy_constructible_v<Counter>);
static_assert(!std::is_move_constructible_v<Counter>);
static_assert(std::is_nothrow_destructible_v<Counter>);
int main() {
    Counter owner(73);
    const auto retained = owner.snapshot();
    if (!owner.Set(91).is_success())
        return 1;
    return readCompiledPeer(owner) == 91 && retained.value == 73 ? 0 : 2;
}
)cpp");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto information = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Entry.pipe", "project_fixture");
    const auto receipt = generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Entry.pipe", "project_fixture", output / "generated",
        information.at("plan_sha256").get<std::string>());
    require(receipt.at("protocol") == "pipelang.native-model-artifacts.v4",
            "compiled fixture did not publish its owning artifact receipt");
}
