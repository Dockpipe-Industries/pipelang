// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"

namespace pipelang::legacy {
fs::path moduleRoot(const fs::path& start) {
    const auto original = fs::absolute(start).lexically_normal();
    for (auto directory = original;; directory = directory.parent_path()) {
        for (const auto* name : {"config.pipe", "config.yml"}) {
            const auto path = directory / name;
            if (fs::exists(path)) {
                regularFile(path);
                return directory;
            }
        }
        if (directory == directory.parent_path())
            return original;
    }
}
std::map<std::string, std::string> readSources(const fs::path& root) {
    require(fs::is_directory(root) && fs::weakly_canonical(root) == root, "source root must be an absolute nonsymbolic directory");
    std::map<std::string, std::string> files;
    std::size_t entries = 0;
    std::size_t bytes = 0;
    for (auto iterator = fs::recursive_directory_iterator(root); iterator != fs::recursive_directory_iterator(); ++iterator) {
        require(++entries <= 16384, "source traversal bound exceeded");
        if (iterator->path().filename() == ".pipelang" && iterator->is_directory()) {
            iterator.disable_recursion_pending();
            continue;
        }
        require(!iterator->is_symlink(), "symbolic source entries are not admitted");
        if (iterator->is_directory() || iterator->path().extension() != ".pipe")
            continue;
        require(files.size() < 128, "legacy source-set bound exceeded");
        auto source = readFile(iterator->path(), 65536 - bytes);
        bytes += source.size();
        files.emplace(iterator->path().generic_string(), std::move(source));
    }
    require(!files.empty(), "no .pipe files found under " + root.string());
    return files;
}
}
