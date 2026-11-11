// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompiledProjectFiles.hpp"
#include "../model-compiler/ResourceBudget.hpp"

#include <algorithm>
#include <charconv>

namespace pipelang::tools {
namespace {

const std::set<std::string> fixedNames{
    "StandaloneConstants.hpp", "Model.hpp", "Types.hpp", "Numeric.hpp", "Objects.hpp", "model.json", "bindings.json",
    "CompositionPlan.json", "CompositionFactory.hpp"
};

bool implementationName(const std::string& name, std::size_t maximumUnits = models::resourceBudget().implementationUnits) {
    if (!name.starts_with("Implementation_") || !name.ends_with(".cpp"))
        return false;
    const auto ordinal = name.substr(15, name.size() - 19);
    if (ordinal.empty() || (ordinal.size() > 1 && ordinal.front() == '0'))
        return false;
    if (!std::all_of(ordinal.begin(), ordinal.end(), [](char value) { return value >= '0' && value <= '9'; }))
        return false;
    std::size_t index = 0;
    const auto parsed = std::from_chars(ordinal.data(), ordinal.data() + ordinal.size(), index);
    return parsed.ec == std::errc{} && parsed.ptr == ordinal.data() + ordinal.size() && index < maximumUnits;
}

bool ownedName(const std::string& name, std::size_t maximumUnits = models::resourceBudget().implementationUnits) {
    return fixedNames.contains(name) || implementationName(name, maximumUnits);
}

}

Json compiledProjectPlan(const Json& implementation, bool withContents) {
    exactObject(implementation, {"protocol", "emission", "files"}, {}, "compiled project implementation");
    require(implementation.at("protocol") == "pipelang.native-project-implementation.v1" &&
            implementation.at("emission") == (withContents ? "sources" : "plan"),
            "compiled project implementation protocol or emission mismatch");
    const auto& files = implementation.at("files");
    require(files.is_array() && !files.empty() && files.size() - 1 <= models::resourceBudget().implementationUnits,
            "compiled project implementation_units resource budget exceeded; configure implementation_units");
    auto plan = Json::array();
    std::set<std::string> names;
    std::size_t units = 0;
    for (const auto& file : files) {
        std::set<std::string> fields{"name", "kind"};
        if (withContents)
            fields.insert("content");
        exactObject(file, fields, {}, "compiled project file");
        const auto name = text(file.at("name"), "compiled project filename", 64);
        require(names.insert(name).second, "duplicate compiled project filename");
        const bool privateHeader = name == "Objects.hpp";
        require(privateHeader || implementationName(name), "unowned compiled project filename");
        require(file.at("kind") == (privateHeader ? "private-header" : "implementation"),
                "compiled project file kind mismatch");
        if (!privateHeader)
            ++units;
        if (withContents)
            text(file.at("content"), "compiled project file content", models::resourceBudget().outputBytes);
        plan.push_back({{"name", name}, {"kind", file.at("kind")}});
    }
    require(names.contains("Objects.hpp"), "compiled project requires its private declarations");
    for (std::size_t index = 0; index < units; ++index)
        require(names.contains("Implementation_" + std::to_string(index) + ".cpp"),
                "compiled project implementation ordinals must be contiguous");
    return plan;
}

std::set<std::string> compiledProjectOwnedNames(const fs::path& output) {
    require(output.is_absolute() && !symbolicPath(output), "output must be an absolute nonsymbolic directory");
    require(fs::weakly_canonical(output) == output.lexically_normal(), "output has symbolic ancestors");
    std::set<std::string> owned;
    if (!fs::exists(output))
        return owned;
    require(fs::is_directory(output), "output must be a directory");
    if (fs::is_empty(output))
        return owned;
    const auto receipt = parseJson(readFile(output / "bindings.json", models::resourceBudget().outputBytes), models::resourceBudget().outputBytes);
    require(receipt.is_object() && receipt.value("protocol", std::string{}) == "pipelang.native-model-artifacts.v4" &&
            receipt.contains("generated") && receipt.at("generated").is_object(),
            "compiled project output requires its prior ownership receipt");
    const auto previousBudget = receipt.contains("resource_budget")
        ? models::ResourceBudget::fromJson(receipt.at("resource_budget").at("profile")) : models::ResourceBudget{};
    const auto fixedCount = fixedNames.size() - 1;
    require(receipt.at("generated").size() <= fixedCount ||
            receipt.at("generated").size() - fixedCount <= previousBudget.implementationUnits,
            "compiled project receipt exceeds its implementation_units resource budget");
    owned.insert("bindings.json");
    for (const auto& [name, digest] : receipt.at("generated").items()) {
        require(ownedName(name, previousBudget.implementationUnits) && name != "bindings.json", "unowned compiled project receipt filename");
        const auto expected = text(digest, "generated artifact digest", 64);
        if (fs::exists(output / name))
            require(fileHash(output / name) == expected,
                    "compiled project output was modified outside its publication");
        owned.insert(name);
    }
    for (const auto& file : fs::directory_iterator(output))
        require(owned.contains(file.path().filename().string()) && fs::is_regular_file(file.symlink_status()),
                "compiled project directory contains unowned material");
    return owned;
}

void publishCompiledProject(const fs::path& output, const std::map<std::string, std::string>& files) {
    auto owned = compiledProjectOwnedNames(output);
    for (const auto& [name, bytes] : files) {
        (void)bytes;
        require(ownedName(name), "unowned compiled project publication filename");
        owned.insert(name);
    }
    publishOwnedDirectory(output, files, owned, true);
}

}
