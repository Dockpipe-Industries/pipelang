// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "manifest.hpp"
#include <algorithm>

namespace pipelang::tools {
void exactHash(const std::string& value) {
    require(value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    }), "expected an exact lowercase SHA256");
}
namespace {
std::string within(const fs::path& prefix, const fs::path& path) {
    require(prefix.is_absolute() && path.is_absolute(), "SDK paths must be absolute");
    const auto relative = path.lexically_relative(prefix).generic_string();
    portablePath(relative);
    require(fs::weakly_canonical(prefix / relative) == (prefix / relative).lexically_normal(), "SDK entry has symbolic ancestors");
    return relative;
}
}
Json inventory(const fs::path& prefix, const Json& roots, const fs::path& excluded) {
    require(roots.is_array() && !roots.empty() && roots.size() <= 16, "invalid SDK root inventory");
    std::map<std::string, Json> records;
    std::set<std::string> selectedRoots;
    for (const auto& entry : roots) {
        const auto name = text(entry, "SDK root");
        portablePath(name);
        require(selectedRoots.insert(name).second, "duplicate SDK root");
        const auto root = prefix / name;
        within(prefix, root);
        require(fs::exists(root) && !symbolicPath(root), "SDK root is missing or symbolic");
        const auto add = [&](const fs::path& path) {
            require(!symbolicPath(path), "SDK symbolic entry refused");
            if (fs::is_directory(path))
                return;
            regularFile(path);
            if (path == excluded)
                return;
            const auto relative = within(prefix, path);
            require(records.size() < 4096 || records.contains(relative), "SDK file inventory exceeds capacity");
            records[relative] = {{"path", relative}, {"sha256", fileHash(path)}, {"bytes", fs::file_size(path)}};
        };
        if (fs::is_directory(root)) {
            for (const auto& path : fs::recursive_directory_iterator(root))
                add(path.path());
        } else {
            add(root);
        }
    }
    Json result = Json::array();
    for (const auto& [name, record] : records)
        result.push_back(record);
    require(!result.empty(), "SDK inventory cannot be empty");
    return result;
}
Json recordManifest(const fs::path& prefix, const fs::path& manifest, const Json& roots) {
    within(prefix, manifest);
    const Json value{{"protocol", "pipelang.native-model-sdk.v1"}, {"version", "0.1.0"},
                     {"roots", roots}, {"files", inventory(prefix, roots, manifest)}};
    const auto bytes = canonical(value, true);
    require(bytes.size() <= 16U << 20, "SDK manifest exceeds its capacity");
    atomicWrite(manifest, bytes);
    return value;
}
Json verifyManifest(const fs::path& prefix, const fs::path& manifest, const std::string& expected) {
    exactHash(expected);
    within(prefix, manifest);
    const auto bytes = readFile(manifest, 16U << 20);
    require(sha256(bytes) == expected, "SDK manifest checksum mismatch");
    const auto value = parseJson(bytes);
    exactObject(value, {"protocol", "version", "roots", "files"}, {}, "SDK manifest");
    require(value.at("protocol") == "pipelang.native-model-sdk.v1" && value.at("version") == "0.1.0", "unexpected SDK protocol or exact version");
    const auto& files = value.at("files");
    require(files.is_array() && !files.empty() && files.size() <= 4096, "invalid SDK file inventory");
    std::set<std::string> names;
    for (const auto& file : files) {
        exactObject(file, {"path", "sha256", "bytes"}, {}, "SDK file");
        const auto name = text(file.at("path"), "SDK file path");
        portablePath(name);
        exactHash(text(file.at("sha256"), "SDK file checksum"));
        require(file.at("bytes").is_number_unsigned() || (file.at("bytes").is_number_integer() && file.at("bytes").get<std::int64_t>() >= 0), "invalid SDK file size");
        require(names.insert(name).second, "duplicate SDK file identity");
    }
    require(inventory(prefix, value.at("roots"), manifest) == files, "installed SDK content differs from its pinned manifest");
    return value;
}
}
