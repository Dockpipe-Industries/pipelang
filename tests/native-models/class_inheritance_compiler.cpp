// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace models = pipelang::models;
using Json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
    require(output.good(), "cannot write " + path.string());
}

std::size_t refusals = 0;

void refuse(const std::string& source, const std::string& diagnostic) {
    try {
        models::compileObjects(source + " public Class App {}", {"App"}, "refused");
    } catch (const models::Diagnostic& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
                "unexpected diagnostic: " + std::string(error.what()) + "; expected " + diagnostic);
        require(error.offset <= source.size() + 20, "diagnostic offset is outside source");
        ++refusals;
        return;
    }
    throw std::runtime_error("accepted invalid source: " + source);
}

void refusalContracts() {
    refuse("public Class A : B {} public Class B : A {}", "cycle");
    refuse("public Class A : A {}", "cycle");
    refuse("public Class A {} public Class B {} public Class C : A, B {}", "one nongeneric base");
    refuse("public Class A {} public Class C : A, A {}", "one nongeneric base");
    refuse("public Class C : Missing {}", "source interface");
    refuse("public Record A {} public Class C : A {}", "source interface");
    refuse("public Interface A { Task<Result<int,string>> Read(); } public Class C : A {}", "source interface");
    refuse("public Class C : App {}", "model roots");
    refuse("public source Interface I {} public Class A {} public Class B : I, A {}", "precede");
    refuse("public Class A { public int value; } public Class B : A { public int value; }", "hidden");
    refuse("public Class A { private int value; } public Class B : A { public int value; }", "hidden");
    refuse("public Class A { public int value; } public Class B : A { public pure int value() => 1; }", "hidden");
    refuse("public Class A { public pure int Read() => 1; } public Class B : A { public int Read; }", "hidden");
    refuse("public Class A { public pure int Read() => 1; } public Class B : A { public pure int Read() => 2; }", "explicit override");
    refuse("public Class A { public pure int Read() => 1; } public Class B : A { public override pure int Read() => 2; }", "virtual");
    refuse("public Class A { public virtual pure int Read() => 1; } public Class B : A { public pure int Read() => 2; }", "explicit override");
    refuse("public Class A { public override pure int Read() => 1; }", "no inherited");
    refuse("public Class A { private virtual pure int Read() => 1; }", "accessible");
    refuse("public Class A { public virtual A() {} }", "instance methods");
    refuse("public Class A { public virtual int value; }", "field declaration");
    refuse("public Class A { public virtual pure T Read<T>(T value) => value; }", "generic virtual");
    refuse("public Class A { public virtual pure int Read() => 1; } public Class B : A { public override int Read() => 2; }", "exact return");
    refuse("public Class A { public virtual pure int Read() => 1; } public Class B : A { protected override pure int Read() => 2; }", "exact return");
    refuse("public Class A { public virtual pure int Read() => 1; } public Class B : A { public override pure float Read() => 2.0; }", "exact return");
    refuse("public Class A { public virtual pure int Read(int value) => value; } public Class B : A { public override pure int Read() => 1; }", "no inherited");
    refuse("public Class A { private int secret; } public Class B : A { public pure int Read() => secret; }", "private");
    refuse("public Class A { private pure int Hidden() => 1; } public Class B : A { public pure int Read() => Hidden(); }", "private");
    refuse("public Class A { protected int secret; } public Class C { public pure int Read(A other) => other.secret; }", "private");
    refuse("public Class A { protected pure int Hidden() => 1; } public Class C { public pure int Read(A other) => other.Hidden(); }", "private");
    refuse("public Class A { public A(int value) {} } public Class B : A {}", "no exact matching");
    refuse("public Class A { private A() {} } public Class B : A {}", "private");
    refuse("public Class A { public A() : base() {} }", "requires a base");
    refuse("public Class A { public A(int value) {} } public Class B : A { public int item; public B() : base(item) {} }", "before base");
    refuse("public Class A { public A(int value) {} } public Class B : A { public B() : base(this.Read()) {} public pure int Read() => 1; }", "instance");
    refuse("public Class A { public pure int Read() => base.Read(); }", "derived");
    refuse("public Class A {} public Class B : A { public pure A Read() => base; }", "expected '.'");
    refuse("public Class A {} public Class B : A {} public Class C { public pure B Read(A value) => value; }", "expected B");
    refuse("public Class A {} public Class B : A {} public Class C : A {} public Class D { public pure B Read(C value) => value; }", "expected B");
    refuse("public Class A { public virtual pure int Read() => 1; public pure int Call() => Read(); } public Class B : A { public override pure int Read() => Call(); }", "recursive");
    refuse("public Class A { public int value; } public Class B : A { public B(int value) {} }", "shadow");
    refuse("public source Interface I { pure int Read(); } public Class A : I {} public Class B : A { public pure int Read() => 1; }", "implement");
    refuse("public Class A { protected int value; } public Record R { protected int value; }", "protected");
    refuse("public Class A { public virtual void Wait(SemaphoreSlim gate) {} } public Class B : A { public override void Wait(SemaphoreSlim gate) { gate.Wait(); } } public Class C { public async Task<Result<int,string>> Run(A target, SemaphoreSlim gate) { target.Wait(gate); return Result<int,string>.Success(1); } }", "blocking");

    std::string deep = "public Class C0 {} ";
    for (int index = 1; index < 33; ++index)
        deep += "public Class C" + std::to_string(index) + " : C" + std::to_string(index - 1) + " {} ";
    refuse(deep, "depth limit");

    std::string boundary = "public Class C0 {} ";
    for (int index = 1; index < 32; ++index)
        boundary += "public Class C" + std::to_string(index) + " : C" + std::to_string(index - 1) + " {} ";
    require(models::compileObjects(boundary + "public Class App {}", {"App"}, "depth_boundary").at("ok") == true,
            "32-class hierarchy boundary refused");

    std::string wide = "public Class A { ";
    for (int index = 0; index < 150; ++index)
        wide += "public int a" + std::to_string(index) + "; ";
    wide += "} public Class B : A { ";
    for (int index = 0; index < 150; ++index)
        wide += "public int b" + std::to_string(index) + "; ";
    refuse(wide + "}", "inherited member limit");

    std::string members = "public Class A { ";
    for (int index = 0; index < 128; ++index)
        members += "public int a" + std::to_string(index) + "; ";
    members += "} public Class B : A { ";
    for (int index = 0; index < 128; ++index)
        members += "public int b" + std::to_string(index) + "; ";
    require(models::compileObjects(members + "} public Class App {}", {"App"}, "member_boundary").at("ok") == true,
            "256-member hierarchy boundary refused");
}

void legacySelection() {
    const auto legacyNames = models::compile(
        "public Record override { public int value; } public Class Legacy { public int base = 3; public pure int Read() => base; }", "legacy_names");
    require(legacyNames.at("ok") == true, "new keywords changed legacy identifier admission");
    for (const auto& source : {
             "public Class A {} public Class B : A {}",
             "public Class A { public virtual pure int Read() => 1; }",
             "public Class A { protected int value; }"}) {
        bool refusedV1 = false;
        bool refusedV2 = false;
        try {
            models::compile(source, "legacy");
        } catch (const models::Diagnostic&) {
            refusedV1 = true;
        }
        try {
            models::compileModules({{"main", std::string("module main; ") + source}}, "main", "legacy");
        } catch (const models::Diagnostic&) {
            refusedV2 = true;
        }
        require(refusedV1 && refusedV2, "inheritance escaped explicit v3 selection");
    }
}

void emit(const std::filesystem::path& output, const std::string& stem, const Json& response, const Json& request) {
    require(response.at("ok") == true, "compiler did not accept fixture");
    write(output / (stem + ".hpp"), response.at("header").get<std::string>());
    write(output / (stem + ".expected.json"), response.dump());
    write(output / (stem + ".request.json"), request.dump());
}

void sourceAndModules(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto source = read(root / "tests/native-models/ClassInheritance.pipe");
    const auto response = models::compileObjects(source, {"App"}, "class_inheritance");
    emit(output, "ClassInheritance", response,
         {{"protocol", "pipelang.native-model.v3"}, {"source", source}, {"models", {"App"}}, {"namespace", "class_inheritance"}});

    const std::string library = R"pipe(module library;
        public source Interface IRead { pure int Read(); }
        public Class Base : IRead {
            protected int count;
            protected Base(int initial) { count = initial; }
            public virtual pure int Read() => count;
        }
    )pipe";
    const std::string application = R"pipe(module main; import library;
        public Record count {}
        public Class Child : Base {
            public Child(int initial) : base(initial) {}
            public override pure int Read() => count + 8;
            public pure int BaseRead() => base.Read();
        }
        public Class App {
            private Base item = new Child(4);
            public pure int Read() => item.Read();
        }
    )pipe";
    const auto moduleResponse = models::compileModuleObjects({{"library", library}, {"main", application}}, "main", {"App"}, "class_modules");
    emit(output, "ClassModules", moduleResponse,
         {{"protocol", "pipelang.native-model.v3"}, {"entry", "main"}, {"models", {"App"}}, {"namespace", "class_modules"},
          {"modules", {{{"name", "library"}, {"source", library}}, {{"name", "main"}, {"source", application}}}}});

    const auto& manifest = response.at("manifest");
    require(manifest.at("protocol") == "pipelang.native-model-manifest.v2", "manifest selection changed");
    const auto& objects = manifest.at("objects");
    bool checkedDeep = false;
    for (const auto& object : objects) {
        if (object.at("name") != "Deep")
            continue;
        require(object.at("implements") == Json::array({"IReadable"}), "inherited interface was lost");
        bool inheritedField = false;
        bool inheritedMethod = false;
        std::size_t readSlots = 0;
        for (const auto& field : object.at("fields")) {
            inheritedField = inheritedField || field.at("name") == "Tag";
            require(field.at("name") != "count" && field.at("name") != "secret", "nonpublic storage escaped manifest");
        }
        for (const auto& method : object.at("methods")) {
            inheritedMethod = inheritedMethod || method.at("name") == "Fixed";
            if (method.at("name") == "Read")
                ++readSlots;
        }
        require(inheritedField && inheritedMethod && readSlots == 1, "manifest lost or duplicated effective members");
        checkedDeep = true;
    }
    require(checkedDeep, "derived object missing from manifest");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--verify") {
            const auto expected = Json::parse(read(argv[2]));
            const auto actual = Json::parse(read(argv[3]));
            require(actual == expected, "installed compiler response differs from C++ API oracle");
            return 0;
        }
        require(argc == 3, "usage: compiler-tests SOURCE_ROOT OUTPUT or --verify EXPECTED ACTUAL");
        const std::filesystem::path output = argv[2];
        std::filesystem::create_directories(output);
        refusalContracts();
        legacySelection();
        sourceAndModules(argv[1], output);
        write(output / "compiler-acceptance.json", Json({{"refusals", refusals}, {"legacy_refusals", 6}, {"source_and_module", true}}).dump(2) + "\n");
        std::cout << refusals << " class-inheritance refusals; source/module/API contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
