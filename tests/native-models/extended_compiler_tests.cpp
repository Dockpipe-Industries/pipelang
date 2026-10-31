// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include <iostream>

namespace {
using namespace pipelang::tools;
class Oracle {
public:
    Oracle(fs::path root, fs::path sdk, fs::path output)
        : root_(std::move(root)), sdk_(std::move(sdk)), output_(std::move(output)) {
        require(!fs::exists(output_), "extended oracle requires fresh output");
        fs::create_directories(output_);
    }
    Json invoke(const Json& request, bool view = false) {
        const auto compiler = sdk_ / (view ? "bin/pipelang-model-viewc" : "bin/pipelang-modelc");
        const auto result = process({compiler.string()}, canonical(request), std::chrono::seconds(15), 8U << 20);
        require(result.exit == 0 && result.diagnostic.empty(), "compiler transport failed: " + result.diagnostic);
        const auto response = parseJson(result.output, 8U << 20);
        require(response.at("protocol") == request.at("protocol"), "compiler response protocol differs");
        return response;
    }
    Json admitted(const std::string& name, const Json& request, bool view = false) {
        const auto response = invoke(request, view);
        require(response.at("ok") == true && response.contains("header") && response.contains("manifest"),
            "expected admission " + name + ": " + response.dump());
        require(response == invoke(request, view), "nondeterministic generation: " + name);
        atomicWrite(output_ / (name + ".hpp"), response.at("header").get<std::string>());
        atomicWrite(output_ / (name + ".manifest.json"), canonical(response.at("manifest")));
        record(name, request, "admitted");
        return response;
    }
    void refused(const std::string& name, const Json& request, bool view = false) {
        const auto response = invoke(request, view);
        require(response.at("ok") == false && !response.contains("header") && !response.contains("manifest") &&
            response.at("diagnostics").is_array() && !response.at("diagnostics").empty(), "expected refusal " + name + ": " + response.dump());
        for (const auto& diagnostic : response.at("diagnostics")) {
            require(!text(diagnostic.at("message"), "diagnostic").empty(), "empty refusal diagnostic");
            const auto position = diagnostic.at(view ? "start" : "offset").get<std::int64_t>();
            require(position >= 0, "negative source diagnostic position");
        }
        record(name, request, "refused");
    }
    std::string source(const std::string& path) const { return readFile(root_ / path, 65536); }
    void finish() const {
        atomicWrite(output_ / "acceptance.json", canonical({{"status", "passed"}, {"cases", cases_}}));
        std::cout << "extended model/view oracle: " << cases_.size() << " cases passed\n";
    }
private:
    void record(const std::string& name, const Json& request, const std::string& result) {
        cases_.push_back({{"id", name}, {"input_sha256", sha256(canonical(request))}, {"outcome", result}});
    }
    fs::path root_, sdk_, output_;
    Json cases_ = Json::array();
};
Json sourceRequest(const std::string& source, const std::string& space, bool objects = false) {
    Json request{{"protocol", objects ? "pipelang.native-model.v3" : "pipelang.native-model.v1"},
        {"source", source}, {"namespace", space}};
    if (objects)
        request["models"] = {"App"};
    return request;
}
std::string replace(std::string source, const std::string& old, const std::string& replacement) {
    const auto position = source.find(old);
    require(position != std::string::npos, "test mutation did not match");
    source.replace(position, old.size(), replacement);
    return source;
}
void run(Oracle& oracle) {
    const auto data = [&](const std::string& name) { return oracle.source("tests/native-models/data/" + name + ".pipe"); };
    for (const auto& [name, space] : std::map<std::string, std::string>{
        {"Collections", "pipelang_models"}, {"Parallel", "pipelang_models"}, {"Synchronization", "synchronization_models"},
        {"Objects", "object_models"}, {"ObjectComposition", "object_composition"}, {"InterfaceInheritance", "interface_inheritance"}}) {
        oracle.admitted(name, sourceRequest(oracle.source("tests/native-models/" + name + ".pipe"), space,
            name == "Objects" || name == "ObjectComposition" || name == "InterfaceInheritance"));
    }
    oracle.admitted("Composition", sourceRequest(data("composition-source"), "composition"));
    oracle.admitted("InitializerOverloads", sourceRequest(data("composition-initializer_source"), "initializers"));
    const auto modules = [](const std::string& space, const std::string& entry, const Json& sources, bool objects = false) {
        Json request{{"protocol", objects ? "pipelang.native-model.v3" : "pipelang.native-model.v2"},
            {"namespace", space}, {"entry", entry}, {"modules", sources}};
        if (objects)
            request["models"] = {"App"};
        return request;
    };
    oracle.admitted("Modules", modules("modules", "App", {
        {{"name", "Values"}, {"source", data("composition-library")}}, {{"name", "App"}, {"source", data("composition-application")}}}));
    oracle.admitted("Mixed", modules("mixed", "App", {
        {{"name", "Data"}, {"source", data("mixed-DATA")}}, {{"name", "App"}, {"source", data("mixed-APP")}}}));
    oracle.admitted("Concurrent", modules("concurrent_models", "App", {
        {{"name", "Values"}, {"source", "module Values; public Record Box<T> { public T Value; }"}},
        {{"name", "App"}, {"source", oracle.source("tests/native-models/Concurrent.pipe")}}}));
    oracle.admitted("ObjectModules", modules("object_modules", "Entry", {
        {{"name", "Values"}, {"source", data("objects_source-Values")}}, {{"name", "Entry"}, {"source", data("objects_source-Entry")}}}, true));
    oracle.admitted("InterfaceModules", modules("interface_modules", "Entry", {
        {{"name", "BaseTypes"}, {"source", data("interfaces_source-BaseTypes")}},
        {{"name", "Types"}, {"source", data("interfaces_source-Types")}},
        {{"name", "Entry"}, {"source", data("interfaces_source-Entry")}}}, true));
    const auto refusals = parseJson(oracle.source("tests/native-models/data/semantic-refusals.json"), 1U << 20);
    for (const auto& item : refusals)
        oracle.refused(item.at("id"), item.at("request"));
    for (const auto& name : {"Objects", "InterfaceInheritance"})
        oracle.refused(std::string(name) + "/legacy-protocol", sourceRequest(oracle.source("tests/native-models/" + std::string(name) + ".pipe"), "boundary"));
    oracle.refused("module/missing-import", modules("boundary", "App", {{{"name", "App"}, {"source", "module App; import Missing; public Class App {}"}}}));
    oracle.refused("module/duplicate", modules("boundary", "App", {
        {{"name", "App"}, {"source", "module App; public Class App {}"}}, {{"name", "App"}, {"source", "module App; public Class App {}"}}}));

    const auto model = oracle.admitted("FixtureModel", sourceRequest(oracle.source("tests/native-model-views/FixtureModel.pipe"), "pipelang_models"));
    const auto source = oracle.source("tests/native-model-views/Fixture.pipe");
    Json view{{"protocol", "pipelang.native-model-view.v1"}, {"source", source}, {"namespace", "pipelang_models"},
        {"model_header", "Model.hpp"}, {"model_manifest", model.at("manifest")}};
    const auto plain = oracle.admitted("View", view, true);
    std::size_t mutation = 0;
    for (const auto& [old, value] : std::vector<std::pair<std::string, std::string>>{
        {"model.Draft.Name", "model.Draft.Missing"}, {"model.Draft.Name", "model.Saving"},
        {"model.CanSave()", "model.Fail()"}, {"changed: model.SetName", "clicked: model.SetName"},
        {"clicked: model.Save", "changed: model.Save"}, {"clicked: model.Save", "clicked: model.SetName"},
        {"model: FixtureModel", "alias: FixtureModel"}, {"id: \"save\"", "id: \"name\""},
        {"view FixtureView", "view class"}, {"changed: model.SetName", "changed: model.SetName, changed: model.SetName"}}) {
        auto candidate = view;
        candidate["source"] = replace(source, old, value);
        oracle.refused("view/mutation-" + std::to_string(mutation++), candidate, true);
    }
    auto styled = view;
    styled["source"] = oracle.source("tests/native-model-views/Theme.pipe") + replace(source, "FixtureModel) {", "FixtureModel) style Compact {");
    const auto themed = oracle.admitted("StyledView", styled, true);
    require(themed.at("manifest") == plain.at("manifest"), "style changed view identity");
    styled["style_inspection"] = "pipelang.native-style-descriptor.v1";
    auto inspected = oracle.admitted("InspectedView", styled, true);
    const auto descriptor = inspected.at("style_descriptor");
    require(descriptor.at("style") == "Compact" && descriptor.at("scope") == "resolved-authored" && descriptor.at("properties").size() == 7,
        "resolved style descriptor differs");
    inspected.erase("style_descriptor");
    require(inspected == themed, "inspection changed default wire response");
    const auto styleRefusals = parseJson(oracle.source("tests/native-model-views/style-refusals.json"));
    for (std::size_t index = 0; index < styleRefusals.size(); ++index) {
        auto candidate = view;
        candidate["source"] = styleRefusals[index].at("source").get<std::string>() + "\n" + source;
        oracle.refused("style/refusal-" + std::to_string(index), candidate, true);
    }
    for (const Json& selector : Json::array({false, true, nullptr, 1, Json::array(), Json::object(), "", "pipelang.native-style-descriptor.v2"})) {
        auto candidate = view;
        candidate["style_inspection"] = selector;
        oracle.refused("style/invalid-selector-" + selector.dump(), candidate, true);
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--numeric-guard") {
            using namespace pipelang::tools;
            for (const auto& flag : {"", "-ffast-math", "-ffinite-math-only"}) {
                std::vector<std::string> command{"/usr/bin/c++", "-std=c++20", "-fsyntax-only", "-x", "c++",
                    "-I" + (fs::path(argv[2]) / "include").string(), argv[3]};
                if (*flag)
                    command.push_back(flag);
                const auto result = process(command, {}, std::chrono::seconds(60), 1U << 20);
                if (*flag)
                    require(result.exit != 0 && result.diagnostic.find("require strict IEEE") != std::string::npos, "unsafe floating mode was not refused");
                else
                    require(result.exit == 0, "strict floating mode failed: " + result.diagnostic);
            }
            std::cout << "strict IEEE compilation passed; unsafe modes refused\n";
            return 0;
        }
        pipelang::tools::require(argc == 4, "usage: extended-tests SOURCE_ROOT SDK OUTPUT");
        Oracle oracle(argv[1], argv[2], argv[3]);
        run(oracle);
        oracle.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extended compiler oracle: " << error.what() << '\n';
        return 1;
    }
}
