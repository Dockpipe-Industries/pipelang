// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "source_set.hpp"

namespace pipelang::tools {
namespace {
constexpr std::size_t maximumSourceBytes = 65536;
constexpr std::size_t maximumModules = 128;
constexpr const char* sourceSetProtocol = "pipelang.native-model-source-set.v1";

void nonsymbolicInput(const fs::path& path) {
    require(path.is_absolute(), "source-set inputs must be absolute");
    regularFile(path);
    for (auto ancestor = path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
        require(!symbolicPath(ancestor), "source-set input has symbolic ancestors");
        if (ancestor == ancestor.root_path())
            break;
    }
    require(fs::canonical(path) == path.lexically_normal(), "source-set input has symbolic ancestors");
}
}

SourceSet SourceSet::load(const fs::path& descriptor) {
    nonsymbolicInput(descriptor);
    require(descriptor.generic_string().find_first_of(";\r\n") == std::string::npos,
            "source-set descriptor path cannot contain CMake list or line separators");
    SourceSet result;
    result.descriptor_ = descriptor.lexically_normal();
    result.descriptorBytes_ = readFile(result.descriptor_, maximumSourceBytes);
    const auto value = parseJson(result.descriptorBytes_, maximumSourceBytes);
    exactObject(value, {"protocol", "entry", "modules"}, {"models"}, "model source set");
    require(value.at("protocol") == sourceSetProtocol, "unsupported model source-set protocol");
    result.entry_ = text(value.at("entry"), "entry module", 1024);
    const auto& modules = value.at("modules");
    require(modules.is_array() && !modules.empty() && modules.size() <= maximumModules,
            "source set requires 1..128 declared modules");
    result.compilerProtocol_ = value.contains("models") ? "pipelang.native-model.v3" : "pipelang.native-model.v2";
    if (value.contains("models")) {
        const auto& models = value.at("models");
        require(models.is_array() && !models.empty() && models.size() <= maximumModules,
                "source-set models require 1..128 explicit exports");
        std::set<std::string> names;
        for (const auto& model : models)
            require(names.insert(text(model, "exported model", 1024)).second, "duplicate source-set model export");
        result.models_ = models;
    }
    result.modules_ = Json::array();
    result.records_ = Json::array();
    result.dependencies_.push_back(result.descriptor_);
    result.contents_.push_back(result.descriptorBytes_);
    std::map<std::string, Json> named;
    std::set<std::string> paths;
    for (const auto& module : modules) {
        exactObject(module, {"name", "path"}, {}, "source-set module");
        const auto name = text(module.at("name"), "source module name", 1024);
        const auto path = text(module.at("path"), "source module path", 4096);
        portablePath(path);
        require(path.find_first_of(";\r\n") == std::string::npos,
                "source-set module path cannot contain CMake list or line separators");
        require(paths.insert(path).second, "duplicate source-set module path");
        require(named.emplace(name, module).second, "duplicate source-set module name");
    }
    require(named.contains(result.entry_), "source-set entry is not a declared module");
    std::size_t totalBytes = 0;
    for (const auto& [name, module] : named) {
        const auto relative = module.at("path").get<std::string>();
        const auto path = result.descriptor_.parent_path() / relative;
        require(path != result.descriptor_, "source-set descriptor cannot also be a source module");
        nonsymbolicInput(path);
        const auto bytes = readFile(path, maximumSourceBytes - totalBytes);
        require(!bytes.empty(), "source-set module cannot be empty");
        totalBytes += bytes.size();
        result.modules_.push_back({{"name", name}, {"source", bytes}});
        result.records_.push_back({{"name", name}, {"path", relative},
                                   {"bytes", bytes.size()}, {"sha256", sha256(bytes)}});
        result.dependencies_.push_back(path);
        result.contents_.push_back(bytes);
    }
    result.verifyUnchanged();
    return result;
}

const std::vector<fs::path>& SourceSet::dependencies() const {
    return dependencies_;
}

const std::string& SourceSet::compilerProtocol() const {
    return compilerProtocol_;
}

Json SourceSet::request(const std::string& nameSpace) const {
    Json result{{"protocol", compilerProtocol_}, {"entry", entry_},
                {"modules", modules_}, {"namespace", nameSpace}};
    if (!models_.is_null())
        result["models"] = models_;
    return result;
}

Json SourceSet::receipt() const {
    Json result{{"protocol", sourceSetProtocol}, {"descriptor_sha256", sha256(descriptorBytes_)},
                {"compiler_protocol", compilerProtocol_}, {"entry", entry_}, {"modules", records_}};
    if (!models_.is_null())
        result["models"] = models_;
    return result;
}

Json SourceSet::information() const {
    Json dependencies = Json::array();
    for (const auto& path : dependencies_)
        dependencies.push_back(path.generic_string());
    return {{"protocol", "pipelang.native-model-source-set-info.v1"},
            {"source_set", receipt()}, {"dependencies", dependencies}};
}

void SourceSet::verifyUnchanged() const {
    for (std::size_t index = 0; index < dependencies_.size(); ++index) {
        nonsymbolicInput(dependencies_[index]);
        require(readFile(dependencies_[index], maximumSourceBytes) == contents_[index],
                "source-set input changed during compilation");
    }
}

Json sourceSetInformation(const fs::path& prefix, const fs::path& manifest,
                          const std::string& manifestHash, const fs::path& descriptor) {
    verifyManifest(prefix, manifest, manifestHash);
    const auto sourceSet = SourceSet::load(descriptor);
    sourceSet.verifyUnchanged();
    verifyManifest(prefix, manifest, manifestHash);
    return sourceSet.information();
}
}
