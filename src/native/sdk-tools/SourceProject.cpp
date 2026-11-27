// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "SourceProject.hpp"
#include "SourceProjectFragment.hpp"
#include "../model-compiler/syntax.hpp"
#include "../model-compiler/ScopedResourceBudget.hpp"
#include "ResourceBudgetReceipt.hpp"

#include <algorithm>
#include <limits>

namespace pipelang::tools {
namespace {

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

std::vector<fs::path> discover(const fs::path& sourceRoot, std::size_t& entryCount,
                               std::size_t& sourceCount) {
    admitPath(sourceRoot, true);
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator iterator(sourceRoot), end; iterator != end; ++iterator) {
        require(entryCount < models::resourceBudget().directoryEntries,
                "project directory_entries resource budget exceeded; configure directory_entries");
        ++entryCount;
        require(static_cast<std::size_t>(iterator.depth()) <= models::resourceBudget().directoryDepth,
                "project directory_depth resource budget exceeded; configure directory_depth");
        const auto path = iterator->path();
        require(!symbolicPath(path), "project discovery refuses symbolic or reparse entries");
        if (path.extension() != ".pipe")
            continue;
        admitPath(path, false);
        portablePath(path.lexically_relative(sourceRoot).generic_string());
        require(sourceCount < models::resourceBudget().indexedFiles,
                "project indexed_files resource budget exceeded; configure indexed_files");
        ++sourceCount;
        paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool containsPath(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root).generic_string();
    return path == root || (!relative.empty() && relative != ".." && !relative.starts_with("../"));
}

std::string foldedName(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

std::string metadata(const fs::path& path, std::size_t& indexedBytes) {
    admitPath(path, false);
    const auto limit = models::resourceBudget().indexedBytes;
    require(indexedBytes <= limit && fs::file_size(path) <= limit - indexedBytes,
            "project metadata indexed_bytes resource budget exceeded; configure indexed_bytes");
    auto bytes = readFile(path, limit - indexedBytes);
    indexedBytes += bytes.size();
    return bytes;
}

struct IndexedSource {
    fs::path path;
    std::string identity;
    std::string owner;
    std::string expectedHash;
};
}

SourceProject SourceProject::load(const fs::path& projectRoot, const fs::path& sourceRoot,
                                  const fs::path& entry, const fs::path& sourceLibraries) {
    admitPath(projectRoot, true);
    portablePath(sourceRoot.generic_string());
    portablePath(entry.generic_string());
    require(entry.extension() == ".pipe", "project entry must be a .pipe source file");
    SourceProject result;
    result.budget_ = models::resourceBudget();
    result.projectRoot_ = projectRoot;
    result.sourceRoot_ = sourceRoot;
    result.entryPath_ = entry;
    result.sourceLibraries_ = sourceLibraries;
    result.libraries_ = Json::array();
    const auto directory = projectRoot / sourceRoot;
    std::size_t directoryEntries = 0;
    std::size_t sourceCount = 0;
    std::size_t indexedBytes = 0;
    result.dependencies_ = discover(directory, directoryEntries, sourceCount);
    result.sourceDirectories_.push_back(directory);
    const auto entryPath = directory / entry;
    require(std::binary_search(result.dependencies_.begin(), result.dependencies_.end(), entryPath),
            "project entry was not discovered under the source root");
    result.index_ = Json::array();
    std::map<std::string, std::vector<SourceProjectFragment>> fragments;
    std::vector<IndexedSource> sources;
    for (const auto& path : result.dependencies_) {
        const auto relative = path.lexically_relative(directory).generic_string();
        sources.push_back({path, sourceLibraries.empty() ? relative : "project/" + relative, {}, {}});
    }
    if (!sourceLibraries.empty()) {
        const auto referenceBytes = metadata(sourceLibraries, indexedBytes);
        result.sourceLibrariesHash_ = sha256(referenceBytes);
        result.dependencies_.push_back(sourceLibraries);
        const auto references = parseJson(referenceBytes, result.budget_.indexedBytes);
        exactObject(references, {"protocol", "libraries"}, {}, "source library references");
        require(references.at("protocol") == "pipelang.source-library-references.v1",
                "unsupported source library references protocol");
        const auto& libraries = references.at("libraries");
        require(libraries.is_array() && !libraries.empty() && libraries.size() <= result.budget_.indexedFiles,
                "source libraries indexed_files resource budget exceeded; configure indexed_files");
        std::map<std::string, Json> orderedLibraries;
        for (const auto& reference : libraries) {
            exactObject(reference, {"root", "descriptor", "sha256"}, {}, "source library reference");
            const fs::path root = text(reference.at("root"), "source library root", 4096);
            admitPath(root, true);
            const auto descriptorName = text(reference.at("descriptor"), "source library descriptor", 4096);
            portablePath(descriptorName);
            const auto expectedHash = text(reference.at("sha256"), "source library descriptor hash");
            exactHash(expectedHash);
            const auto descriptorPath = root / descriptorName;
            const auto descriptorBytes = metadata(descriptorPath, indexedBytes);
            require(sha256(descriptorBytes) == expectedHash, "source library descriptor SHA256 mismatch");
            const auto descriptor = parseJson(descriptorBytes, result.budget_.indexedBytes);
            exactObject(descriptor, {"protocol", "name", "version", "source_root", "sources"}, {}, "source library descriptor");
            require(descriptor.at("protocol") == "pipelang.source-library.v1",
                    "unsupported source library descriptor protocol");
            const auto version = text(descriptor.at("version"), "source library version", 128);
            require(std::all_of(version.begin(), version.end(), [](unsigned char character) {
                return character >= 0x21 && character <= 0x7e;
            }), "source library version must be a bounded printable identity");
            const auto name = text(descriptor.at("name"), "source library name", 256);
            require(!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
            }) && name != "." && name != "..", "source library name must be a portable identity");
            require(orderedLibraries.emplace(foldedName(name), Json{{"reference", reference}, {"descriptor", descriptor}}).second,
                    "duplicate source library name");
            result.dependencies_.push_back(descriptorPath);
        }
        for (const auto& [key, library] : orderedLibraries) {
            (void)key;
            const auto& reference = library.at("reference");
            const auto& descriptor = library.at("descriptor");
            const auto name = descriptor.at("name").get<std::string>();
            const fs::path root = reference.at("root").get<std::string>();
            const auto sourceRootName = text(descriptor.at("source_root"), "source library source root", 4096);
            portablePath(sourceRootName);
            const auto libraryDirectory = root / sourceRootName;
            for (const auto& existing : result.sourceDirectories_)
                require(!containsPath(existing, libraryDirectory) && !containsPath(libraryDirectory, existing),
                        "source library roots must not overlap project or library source roots");
            result.sourceDirectories_.push_back(libraryDirectory);
            const auto& inventory = descriptor.at("sources");
            require(inventory.is_array() && !inventory.empty() && inventory.size() <= result.budget_.indexedFiles,
                    "source library inventory indexed_files resource budget exceeded; configure indexed_files");
            std::map<std::string, std::string> pinnedSources;
            std::set<std::string> foldedPaths;
            std::string previous;
            for (const auto& record : inventory) {
                exactObject(record, {"path", "sha256"}, {}, "source library source");
                const auto relative = text(record.at("path"), "source library source path", 4096);
                portablePath(relative);
                require(fs::path(relative).extension() == ".pipe", "source library inventory requires .pipe paths");
                require(previous.empty() || previous < relative, "source library inventory must have unique sorted paths");
                previous = relative;
                require(foldedPaths.insert(foldedName(relative)).second, "source library case-folded source path collision");
                const auto expectedHash = text(record.at("sha256"), "source library source hash");
                exactHash(expectedHash);
                pinnedSources.emplace(relative, expectedHash);
            }
            const auto paths = discover(libraryDirectory, directoryEntries, sourceCount);
            require(paths.size() == pinnedSources.size(), "source library pinned inventory membership mismatch");
            for (const auto& path : paths) {
                const auto relative = path.lexically_relative(libraryDirectory).generic_string();
                require(pinnedSources.contains(relative), "source library discovered unpinned source: " + relative);
                sources.push_back({path, "libraries/" + name + "/" + relative, name, pinnedSources.at(relative)});
                result.dependencies_.push_back(path);
            }
            result.libraries_.push_back({{"name", name}, {"version", descriptor.at("version")}, {"root", root.generic_string()},
                {"source_root", sourceRootName}, {"descriptor", reference.at("descriptor")},
                {"descriptor_sha256", reference.at("sha256")}});
        }
        std::sort(result.dependencies_.begin(), result.dependencies_.end());
        require(std::adjacent_find(result.dependencies_.begin(), result.dependencies_.end()) == result.dependencies_.end(),
                "duplicate source library input path");
    }
    std::map<fs::path, std::string> identities;
    std::map<std::string, std::string> moduleOwners;
    for (const auto& source : sources) {
        const auto& path = source.path;
        identities.emplace(path, source.identity);
        const auto fileBytes = fs::file_size(path);
        require(fileBytes <= std::numeric_limits<std::size_t>::max() &&
                !models::budgetExceeded(indexedBytes, static_cast<std::size_t>(fileBytes), result.budget_.indexedBytes),
                "project indexed_bytes resource budget exceeded; configure indexed_bytes");
        auto bytes = readFile(path, result.budget_.indexedBytes - indexedBytes);
        require(!bytes.empty(), "project source cannot be empty: " + path.generic_string());
        require(!models::budgetExceeded(indexedBytes, bytes.size(), result.budget_.indexedBytes),
                "project indexed_bytes resource budget exceeded; configure indexed_bytes");
        indexedBytes += bytes.size();
        require(source.expectedHash.empty() || sha256(bytes) == source.expectedHash,
                "source library source SHA256 mismatch: " + path.generic_string());
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
        const auto [owner, inserted] = moduleOwners.emplace(module, source.owner);
        if (!inserted) {
            require(owner->second == source.owner,
                    "project module ownership collision: " + module + " in " +
                    fragments.at(module).front().path.generic_string() + " and " + path.generic_string());
        }
        auto& parts = fragments[module];
        if (!parts.empty() && (!parts.front().explicitModule || !explicitModule)) {
            throw std::runtime_error("duplicate project module '" + module + "' in " +
                                     parts.front().path.generic_string() + " and " + path.generic_string() +
                                     "; multiple files require explicit module declarations");
        }
        if (path == entryPath)
            result.entryModule_ = module;
        result.index_.push_back({{"path", source.identity},
                                 {"module", module}, {"explicit_module", explicitModule},
                                 {"bytes", bytes.size()}, {"sha256", sha256(bytes)}});
        parts.push_back({path, std::move(bytes), std::move(header), explicitModule});
    }
    std::set<std::string> visiting;
    std::set<std::string> reachable;
    using Dependency = std::pair<std::string, std::pair<fs::path, std::size_t>>;
    struct ImportFrame {
        std::string module;
        std::vector<Dependency> dependencies;
        std::size_t next = 0;
    };
    auto enter = [&](const std::string& module) {
        require(fragments.contains(module), "missing imported project module: " + module);
        require(reachable.size() < result.budget_.modules,
                "project modules resource budget exceeded; configure modules");
        reachable.insert(module);
        visiting.insert(module);
        std::map<std::string, std::pair<fs::path, std::size_t>> dependencies;
        for (const auto& fragment : fragments.at(module)) {
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
        return ImportFrame{module, {dependencies.begin(), dependencies.end()}, 0};
    };
    require(result.index_.dump().size() <= result.budget_.outputBytes,
            "project index receipt output_bytes resource budget exceeded; configure output_bytes");
    std::vector<ImportFrame> pending;
    pending.push_back(enter(result.entryModule_));
    while (!pending.empty()) {
        auto& frame = pending.back();
        if (frame.next == frame.dependencies.size()) {
            visiting.erase(frame.module);
            pending.pop_back();
            continue;
        }
        const auto [dependency, origin] = frame.dependencies[frame.next++];
        const auto location = origin.first.generic_string() + " at offset " + std::to_string(origin.second);
        require(!visiting.contains(dependency), "project module import cycle: " + dependency + " from " + location);
        require(fragments.contains(dependency), "missing imported project module: " + dependency + " from " + location);
        if (!reachable.contains(dependency))
            pending.push_back(enter(dependency));
    }
    std::size_t closureBytes = 0;
    for (const auto& module : reachable) {
        for (auto& fragment : fragments.at(module)) {
            require(!models::budgetExceeded(closureBytes, fragment.source.size(), result.budget_.inputBytes),
                    "project closure input_bytes resource budget exceeded; configure input_bytes");
            closureBytes += fragment.source.size();
            result.modules_.push_back({module, std::move(fragment.source), identities.at(fragment.path)});
        }
    }
    return result;
}

const std::vector<fs::path>& SourceProject::dependencies() const {
    return dependencies_;
}

const std::vector<fs::path>& SourceProject::sourceDirectories() const {
    return sourceDirectories_;
}

const std::string& SourceProject::entryModule() const {
    return entryModule_;
}

const std::vector<models::ModuleSource>& SourceProject::modules() const {
    return modules_;
}

Json SourceProject::request(const std::string& nameSpace) const {
    Json modules = Json::array();
    for (const auto& fragment : modules_)
        modules.push_back({{"name", fragment.name}, {"path", fragment.path}, {"source", fragment.source}});
    return {{"protocol", "pipelang.native-model-project.v1"}, {"entry", entryModule_},
            {"modules", std::move(modules)}, {"namespace", nameSpace}};
}

Json SourceProject::receipt() const {
    Json closure = Json::array();
    std::set<std::string> modules;
    Json closureSources = Json::array();
    for (const auto& fragment : modules_) {
        modules.insert(fragment.name);
        closureSources.push_back(fragment.path);
    }
    for (const auto& module : modules)
        closure.push_back(module);
    Json result = {{"protocol", "pipelang.native-model-project-inputs.v1"},
                   {"source_root", sourceRoot_.generic_string()}, {"entry_path", entryPath_.generic_string()},
                   {"entry", entryModule_}, {"index", index_}, {"closure", closure},
                   {"closure_sources", closureSources}};
    if (!sourceLibraries_.empty()) {
        result["source_libraries_sha256"] = sourceLibrariesHash_;
        result["source_libraries"] = libraries_;
    }
    require(result.dump().size() <= budget_.outputBytes,
            "project receipt output_bytes resource budget exceeded; configure output_bytes");
    return result;
}

Json SourceProject::information() const {
    Json dependencies = Json::array();
    for (const auto& dependency : dependencies_)
        dependencies.push_back(dependency.generic_string());
    Json result = {{"protocol", "pipelang.native-model-project-info.v1"},
                   {"project", receipt()}, {"dependencies", dependencies}};
    if (!sourceLibraries_.empty()) {
        result["source_directories"] = Json::array();
        for (const auto& directory : sourceDirectories_)
            result["source_directories"].push_back(directory.generic_string());
    }
    result["resource_budget"] = resourceBudgetReceipt(budget_);
    require(result.dump().size() <= budget_.outputBytes,
            "project information output_bytes resource budget exceeded; configure output_bytes");
    return result;
}

void SourceProject::verifyUnchanged() const {
    const models::ScopedResourceBudget scope(budget_);
    const auto current = load(projectRoot_, sourceRoot_, entryPath_, sourceLibraries_);
    require(current.dependencies_ == dependencies_ && current.receipt() == receipt(),
            "project index or source inputs changed during compilation");
}

Json projectInformation(const fs::path& prefix, const fs::path& manifest,
                        const std::string& manifestHash, const fs::path& projectRoot,
                        const fs::path& sourceRoot, const fs::path& entry,
                        const fs::path& sourceLibraries) {
    verifyManifest(prefix, manifest, manifestHash);
    const auto project = SourceProject::load(projectRoot, sourceRoot, entry, sourceLibraries);
    project.verifyUnchanged();
    verifyManifest(prefix, manifest, manifestHash);
    return project.information();
}
}
