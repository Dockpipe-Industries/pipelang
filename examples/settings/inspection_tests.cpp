// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include <iostream>

namespace {
using namespace pipelang::tools;
Json controls(const Json& frame) {
    require(frame.at("schema") == "pipelang.pipeview.inspect.v3", "inspection contract differs");
    Json result = Json::object();
    for (const auto& item : frame.at("controls")) {
        const auto identity = item.at("id").get<std::string>();
        require(!result.contains(identity), "duplicate control identity");
        result[identity] = item;
    }
    return result;
}
void frames(const fs::path& path) {
    const auto report = parseJson(readFile(path, 1U << 20));
    exactObject(report, {"initial", "default", "edited", "saving", "saved", "reverted"}, {}, "inspection frames");
    struct Expected { const char* name; const char* player; bool vsync; const char* volume; bool saving; bool canSave; const char* status; };
    for (const auto& expected : std::vector<Expected>{
        {"initial", "Rider", true, "100", false, false, "Saved"},
        {"edited", "RiderX", false, "99", false, true, "Unsaved changes"},
        {"saving", "RiderX", false, "99", true, false, "Saving"},
        {"saved", "RiderX", false, "99", false, false, "Saved"},
        {"reverted", "RiderX", false, "99", false, false, "Saved"}}) {
        const auto frame = controls(report.at(expected.name));
        for (const auto& [identity, value] : std::map<std::string, Json>{
            {"name-field", expected.player}, {"vsync-toggle", expected.vsync}, {"volume-stepper", expected.volume}, {"save-status", expected.status}}) {
            require(frame.at(identity).at("value") == value && frame.at(identity).at("redacted") == false, "typed value or explicit exposure differs");
        }
        require(frame.at("save-button").at("enabled") == expected.canSave, "Save availability differs");
        for (const auto* identity : {"name-field", "vsync-toggle", "volume-stepper", "revert-button"})
            require(frame.at(identity).at("enabled") == !expected.saving, "pending availability differs");
        require(frame.at("save-error").at("value") == "", "unexpected persistence detail");
    }
    const auto redacted = controls(report.at("default"));
    for (const auto* identity : {"name-field", "vsync-toggle", "volume-stepper"})
        require(redacted.at(identity).at("redacted") == true && !redacted.at(identity).contains("value"), "default inspection disclosed protected data");
    require(redacted.at("save-status").at("value") == "Saved", "public status unavailable");
}
void style(const fs::path& directory) {
    const auto descriptor = parseJson(readFile(directory / "style.json", 65536));
    const Json expected{{"protocol", "pipelang.native-style-descriptor.v1"}, {"view", "SettingsView"}, {"style", "SettingsTheme"},
        {"scope", "resolved-authored"}, {"properties", {
            {{"name", "background"}, {"type", "color"}, {"value", "#142434FF"}},
            {{"name", "controlHeight"}, {"type", "length"}, {"unit", "px"}, {"value", 42}},
            {{"name", "focus"}, {"type", "color"}, {"value", "#66CCFFFF"}},
            {{"name", "gap"}, {"type", "length"}, {"unit", "px"}, {"value", 10}},
            {{"name", "padding"}, {"type", "length"}, {"unit", "px"}, {"value", 16}},
            {{"name", "text"}, {"type", "color"}, {"value", "#EAF1F8FF"}}}}};
    require(descriptor == expected, "authored settings style differs");
    const auto receipt = parseJson(readFile(directory / "bindings.json", 1U << 20));
    exactObject(receipt.at("generated"), {"Model.hpp", "model.json", "ModelView.hpp", "view.json", "style.json"}, {}, "generated files");
    for (const auto& [name, hash] : receipt.at("generated").items())
        require(fileHash(directory / name) == hash, "generated settings file identity differs");
}
}
int main(int argc, char** argv) {
    try {
        pipelang::tools::require(argc == 3, "usage: inspection-tests frames|style INPUT");
        if (std::string(argv[1]) == "frames")
            frames(argv[2]);
        else if (std::string(argv[1]) == "style")
            style(argv[2]);
        else
            throw std::runtime_error("unknown inspection test mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
