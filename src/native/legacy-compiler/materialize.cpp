// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>

namespace pipelang::legacy {
namespace {
std::string trim(std::string value) {
    static const std::vector<std::string> spaces{" ", "\t", "\n", "\r", "\v", "\f", "\xc2\x85", "\xc2\xa0",
        "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84",
        "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a",
        "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        for (const auto& space : spaces) {
            if (value.starts_with(space)) {
                value.erase(0, space.size());
                changed = true;
            }
            if (value.ends_with(space)) {
                value.resize(value.size() - space.size());
                changed = true;
            }
        }
    }
    return value;
}
std::vector<fs::path> pipePaths(const fs::path& root) {
    require(root.is_absolute() && fs::weakly_canonical(root) == root && fs::is_directory(root), "materializer root must be an absolute nonsymbolic directory");
    std::vector<fs::path> paths;
    std::size_t count = 0;
    for (auto iterator = fs::recursive_directory_iterator(root); iterator != fs::recursive_directory_iterator(); ++iterator) {
        require(++count <= 16384, "materializer traversal bound exceeded");
        require(!iterator->is_symlink(), "materializer refuses symbolic source entries");
        if (iterator->is_directory()) {
            if (iterator->path().filename() == ".pipelang")
                iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->path().extension() == ".pipe") {
            regularFile(iterator->path());
            require(paths.size() < 4096, "materializer source inventory exceeds capacity");
            paths.push_back(iterator->path());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
std::map<std::string, std::string> mappings(const fs::path& module) {
    const auto config = module / "config.yml";
    if (!fs::exists(config))
        return {};
    const auto document = YAML::Load(readFile(config, 1U << 20));
    if (!document || document.IsNull())
        return {};
    require(document.IsMap(), "workflow YAML must be a mapping");
    std::set<std::string> keys;
    for (const auto& entry : document) {
        require(entry.first.IsScalar() && keys.insert(entry.first.Scalar()).second, "workflow YAML has duplicate or nonscalar keys");
    }
    const auto types = document["types"];
    if (!types || types.IsNull())
        return {};
    require(types.IsSequence() && types.size() <= 128, "workflow types must be a bounded sequence");
    std::map<std::string, std::string> result;
    for (const auto& type : types) {
        require(type.IsScalar(), "workflow types entries must be scalar strings");
        const auto spec = trim(type.Scalar());
        if (spec.empty())
            continue;
        auto left = spec;
        std::string reference;
        const auto first = spec.find('<');
        const auto last = spec.rfind('>');
        if (first != std::string::npos || last != std::string::npos) {
            require(first != std::string::npos && last != std::string::npos && first > 0 && last > first + 1, "invalid workflow type mapping: " + spec);
            left = trim(spec.substr(0, first));
            reference = trim(spec.substr(first + 1, last - first - 1));
        }
        require(!left.empty(), "empty workflow type mapping path");
        fs::path relative(left);
        if (relative.extension().empty())
            relative += ".pipe";
        if (reference.empty())
            reference = relative.stem().string();
        // filepath.Join in the legacy contract joins these lexical components.
        const auto path = fs::path(module.generic_string() + '/' + relative.generic_string()).lexically_normal();
        result[path.generic_string()] = reference;
    }
    return result;
}
std::string resolve(const Program& program, const std::string& reference) {
    for (const auto& owner : program.declarations) {
        if (!owner.contract && owner.name == reference)
            return owner.name;
    }
    std::vector<std::string> candidates;
    for (const auto& owner : program.declarations) {
        if (!owner.contract && owner.implements == reference)
            candidates.push_back(owner.name);
    }
    require(candidates.size() == 1, "workflow type reference has no unique implementing class: " + reference);
    return candidates.front();
}
bool upToDate(const fs::file_time_type& latest, const fs::path& directory, const std::string& base) {
    for (const auto* extension : {".workflow.yml", ".bindings.json", ".bindings.env"}) {
        const auto path = directory / (base + extension);
        if (!fs::is_regular_file(fs::symlink_status(path)) || fs::last_write_time(path) < latest)
            return false;
    }
    return true;
}
}
std::vector<fs::path> workflowRoots(const fs::path& project) {
    std::vector<fs::path> roots{"workflows"};
    const auto config = project / "dockpipe.config.json";
    if (fs::exists(config)) {
        const auto document = parseJson(readFile(config, 1U << 20));
        require(document.is_null() || document.is_object(), "DockPipe configuration must be a JSON object");
        if (document.is_object() && document.contains("compile") && !document.at("compile").is_null()) {
            const auto& compile = document.at("compile");
            require(compile.is_object(), "compile configuration must be an object");
            if (compile.contains("workflows") && !compile.at("workflows").is_null()) {
                require(compile.at("workflows").is_array() && compile.at("workflows").size() <= 16, "workflow roots must be a bounded array");
                roots.clear();
                for (const auto& value : compile.at("workflows"))
                    roots.emplace_back(text(value, "workflow root"));
            }
        }
    }
    for (auto& root : roots) {
        if (!root.is_absolute())
            root = project / root;
        root = root.lexically_normal();
    }
    return roots;
}
std::vector<fs::path> existingRoots(const std::vector<fs::path>& requested) {
    require(requested.size() <= 16, "materializer root bounds exceeded");
    std::set<std::string> visited;
    std::vector<fs::path> roots;
    for (const auto& value : requested) {
        const auto trimmed = trim(value.string());
        if (trimmed.empty())
            continue;
        const auto root = fs::absolute(trimmed).lexically_normal();
        if (!fs::is_directory(root) || !visited.insert(root.generic_string()).second)
            continue;
        roots.push_back(root);
    }
    return roots;
}
std::size_t materialize(const std::vector<fs::path>& requested, bool force, const fs::path& output) {
    require(output.is_absolute(), "materializer output must be absolute");
    std::size_t count = 0;
    for (const auto& root : existingRoots(requested)) {
        for (const auto& path : pipePaths(root)) {
            const auto module = moduleRoot(path.parent_path());
            const auto files = readSources(module);
            const auto types = mappings(module);
            const auto program = merge(files);
            std::string owner;
            if (!types.empty()) {
                const auto selected = types.find(path.generic_string());
                if (selected == types.end())
                    continue;
                owner = resolve(program, selected->second);
            } else {
                const auto local = parse(files.at(path.generic_string()), path.generic_string());
                for (const auto& declaration : local.declarations) {
                    if (!declaration.contract) {
                        owner = declaration.name;
                        break;
                    }
                }
            }
            if (owner.empty())
                continue;
            const auto artifacts = compile(program, owner);
            const auto directory = (output / sha256(root.generic_string()).substr(0, 16) / path.parent_path().lexically_relative(root)).lexically_normal();
            const auto base = path.stem().string() + '.' + artifacts.entry;
            auto latest = fs::file_time_type::min();
            for (const auto& [name, source] : files)
                latest = std::max(latest, fs::last_write_time(name));
            if (!force && upToDate(latest, directory, base))
                continue;
            publishArtifacts(directory, base, artifacts);
            ++count;
        }
    }
    return count;
}
}
