// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "SourceProject.hpp"
#include "SourceProjectFragment.hpp"
#include "../model-compiler/syntax.hpp"

#include <algorithm>

namespace pipelang::tools {
namespace {
constexpr std::size_t maximumIndexedFiles = 4096;
constexpr std::size_t maximumIndexedBytes = 16U << 20;
constexpr std::size_t maximumDirectoryEntries = 32768;
constexpr std::size_t maximumDirectoryDepth = 32;

void admitPath(const fs::path& path, bool directory) {
    require(path.is_absolute() && path == path.lexically_normal(),
            "project paths must be absolute and normalized");
    require(path.generic_string().find_first_of(";\r\n") == std::string::npos,
            "project paths cannot contain CMake list or line separators");
    require(path.generic_string().find_first_of("[]*?") == std::string::npos,
            "project paths cannot contain CMake glob metacharacters");
    require(path.generic_string().find("$<") == std::string::npos,
            "project paths cannot contain CMake generator expressions");
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        require(!symbolicPath(ancestor), "project inputs cannot contain symbolic or reparse paths");
        if (ancestor == ancestor.root_path())
            break;
    }
    require(fs::canonical(path) == path, "project input canonical path mismatch");
    if (directory)
        require(fs::is_directory(path), "project root must be a directory");
    else
        regularFile(path);
}

std::vector<fs::path> discover(const fs::path& sourceRoot) {
    admitPath(sourceRoot, true);
    std::vector<fs::path> paths;
    std::size_t entryCount = 0;
    for (fs::recursive_directory_iterator iterator(sourceRoot), end; iterator != end; ++iterator) {
        require(++entryCount <= maximumDirectoryEntries, "project directory entry limit exceeded");
        require(iterator.depth() <= static_cast<int>(maximumDirectoryDepth), "project directory depth limit exceeded");
        const auto path = iterator->path();
        require(!symbolicPath(path), "project discovery refuses symbolic or reparse entries");
        if (path.extension() != ".pipe")
            continue;
        admitPath(path, false);
        portablePath(path.lexically_relative(sourceRoot).generic_string());
        paths.push_back(path);
        require(paths.size() <= maximumIndexedFiles, "project index file limit exceeded");
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
}

SourceProject SourceProject::load(const fs::path& projectRoot, const fs::path& sourceRoot,
                                  const fs::path& entry) {
    admitPath(projectRoot, true);
    portablePath(sourceRoot.generic_string());
    portablePath(entry.generic_string());
    require(entry.extension() == ".pipe", "project entry must be a .pipe source file");
    SourceProject result;
    result.projectRoot_ = projectRoot;
    result.sourceRoot_ = sourceRoot;
    result.entryPath_ = entry;
    const auto directory = projectRoot / sourceRoot;
    result.dependencies_ = discover(directory);
    const auto entryPath = directory / entry;
    require(std::binary_search(result.dependencies_.begin(), result.dependencies_.end(), entryPath),
            "project entry was not discovered under the source root");
    result.index_ = Json::array();
    result.modules_ = Json::array();
    std::map<std::string, std::vector<SourceProjectFragment>> fragments;
    std::size_t indexedBytes = 0;
    for (const auto& path : result.dependencies_) {
        const auto bytes = readFile(path, models::sourceLimit);
        require(!bytes.empty(), "project source cannot be empty: " + path.generic_string());
        require(bytes.size() <= maximumIndexedBytes - indexedBytes, "project index byte limit exceeded");
        indexedBytes += bytes.size();
        models::Program header;
        bool explicitModule = false;
        try {
            header = models::parseModuleHeader(bytes);
            explicitModule = !header.module.empty();
            if (header.module.empty())
                header.module = models::moduleIdentityFromStem(path.stem().string());
        } catch (const models::Diagnostic& diagnostic) {
            throw std::runtime_error("project source " + path.generic_string() + " at offset " +
                                     std::to_string(diagnostic.offset) + ": " + diagnostic.what());
        }
        const auto module = header.module;
        auto& parts = fragments[module];
        if (!parts.empty() && (!parts.front().explicitModule || !explicitModule)) {
            throw std::runtime_error("duplicate project module '" + module + "' in " +
                                     parts.front().path.generic_string() + " and " + path.generic_string() +
                                     "; multiple files require explicit module declarations");
        }
        parts.push_back({path, bytes, std::move(header), explicitModule});
        if (path == entryPath)
            result.entryModule_ = module;
        result.index_.push_back({{"path", path.lexically_relative(directory).generic_string()},
                                 {"module", module}, {"explicit_module", explicitModule},
                                 {"bytes", bytes.size()}, {"sha256", sha256(bytes)}});
    }
    std::set<std::string> visiting;
    std::set<std::string> reachable;
    std::size_t closureBytes = 0;
    std::function<void(const std::string&)> visit = [&](const std::string& module) {
        require(!visiting.contains(module), "project module import cycle: " + module);
        if (reachable.contains(module))
            return;
        require(fragments.contains(module), "missing imported project module: " + module);
        require(reachable.size() < 128, "project import closure exceeds 128 modules");
        reachable.insert(module);
        visiting.insert(module);
        std::map<std::string, std::pair<fs::path, std::size_t>> dependencies;
        for (const auto& fragment : fragments.at(module)) {
            require(fragment.source.size() <= models::sourceLimit - closureBytes,
                    "project import closure exceeds 65536 source bytes");
            closureBytes += fragment.source.size();
            std::set<std::string> imports;
            for (std::size_t index = 0; index < fragment.header.imports.size(); ++index) {
                const auto& dependency = fragment.header.imports[index];
                require(imports.insert(dependency).second,
                        "duplicate project import: " + dependency + " in " +
                        fragment.path.generic_string() + " at offset " +
                        std::to_string(fragment.header.importOffsets.at(index)));
                require(dependency != module,
                        "project module import cycle through self import: " + dependency + " in " + fragment.path.generic_string());
                dependencies.emplace(dependency, std::make_pair(fragment.path, fragment.header.importOffsets.at(index)));
            }
        }
        for (const auto& [dependency, origin] : dependencies) {
            const auto location = origin.first.generic_string() + " at offset " + std::to_string(origin.second);
            require(!visiting.contains(dependency), "project module import cycle: " + dependency + " from " + location);
            require(fragments.contains(dependency), "missing imported project module: " + dependency + " from " + location);
            visit(dependency);
        }
        visiting.erase(module);
    };
    require(result.index_.dump().size() <= (4U << 20), "project index receipt byte limit exceeded");
    visit(result.entryModule_);
    for (const auto& module : reachable) {
        for (const auto& fragment : fragments.at(module)) {
            result.modules_.push_back({{"name", module}, {"source", fragment.source},
                {"path", fragment.path.lexically_relative(directory).generic_string()}});
        }
    }
    return result;
}

const std::vector<fs::path>& SourceProject::dependencies() const {
    return dependencies_;
}

Json SourceProject::request(const std::string& nameSpace) const {
    return {{"protocol", "pipelang.native-model-project.v1"}, {"entry", entryModule_},
            {"modules", modules_}, {"namespace", nameSpace}};
}

Json SourceProject::receipt() const {
    Json closure = Json::array();
    std::set<std::string> modules;
    Json closureSources = Json::array();
    for (const auto& fragment : modules_) {
        modules.insert(fragment.at("name").get<std::string>());
        closureSources.push_back(fragment.at("path"));
    }
    for (const auto& module : modules)
        closure.push_back(module);
    Json result = {{"protocol", "pipelang.native-model-project-inputs.v1"},
                   {"source_root", sourceRoot_.generic_string()}, {"entry_path", entryPath_.generic_string()},
                   {"entry", entryModule_}, {"index", index_}, {"closure", closure},
                   {"closure_sources", closureSources}};
    require(result.dump().size() <= (4U << 20), "project input receipt byte limit exceeded");
    return result;
}

Json SourceProject::information() const {
    Json dependencies = Json::array();
    for (const auto& dependency : dependencies_)
        dependencies.push_back(dependency.generic_string());
    Json result = {{"protocol", "pipelang.native-model-project-info.v1"},
                   {"project", receipt()}, {"dependencies", dependencies}};
    require(result.dump().size() <= (4U << 20), "project information receipt byte limit exceeded");
    return result;
}

void SourceProject::verifyUnchanged() const {
    const auto current = load(projectRoot_, sourceRoot_, entryPath_);
    require(current.dependencies_ == dependencies_ && current.receipt() == receipt(),
            "project index or source inputs changed during compilation");
}

Json projectInformation(const fs::path& prefix, const fs::path& manifest,
                        const std::string& manifestHash, const fs::path& projectRoot,
                        const fs::path& sourceRoot, const fs::path& entry) {
    verifyManifest(prefix, manifest, manifestHash);
    const auto project = SourceProject::load(projectRoot, sourceRoot, entry);
    project.verifyUnchanged();
    verifyManifest(prefix, manifest, manifestHash);
    return project.information();
}
}
