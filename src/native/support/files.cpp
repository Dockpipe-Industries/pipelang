// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pipelang::tools {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool symbolicPath(const fs::path& path) {
#ifdef _WIN32
    return windowsReparsePoint(path);
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}
void regularFile(const fs::path& path) {
#ifdef _WIN32
    require(!windowsReparsePoint(path), "expected a regular nonsymbolic file: " + path.string());
#endif
    require(fs::is_regular_file(fs::symlink_status(path)), "expected a regular nonsymbolic file: " + path.string());
}
std::string readFile(const fs::path& path, std::size_t maximum) {
    regularFile(path);
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open input: " + path.string());
    std::string result;
    std::array<char, 8192> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        require(count <= maximum - result.size(), "input exceeds its byte limit: " + path.string());
        result.append(buffer.data(), count);
    }
    require(input.eof(), "failed to read input: " + path.string());
    return result;
}
std::string uniqueName() {
    std::random_device random;
    return std::to_string(random()) + std::to_string(random()) + std::to_string(random());
}
void portablePath(const std::string& value) {
    require(!value.empty() && value.find('\\') == std::string::npos && value.find('\0') == std::string::npos,
            "SDK paths must be portable relative paths");
    const fs::path path(value);
    require(!path.has_root_path() && path.generic_string() == value, "SDK path is not canonical");
    for (const auto& component : path)
        require(component != "." && component != ".." && !component.empty(), "SDK path escapes its prefix");
    require(value.find(":") == std::string::npos && value.find("//") == std::string::npos && value.back() != '/',
            "SDK path is not portable or canonical");
}
void atomicWrite(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (".pipelang-" + uniqueName());
    try {
#ifdef _WIN32
        atomicWriteWindows(temporary, path, bytes);
#else
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(output.good(), "failed to write staged output");
        const int file = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
        require(file >= 0, "cannot open staged output for synchronization");
        const int synced = ::fsync(file);
        ::close(file);
        require(synced == 0, "cannot synchronize staged output");
        fs::rename(temporary, path);
#endif
    } catch (...) {
        fs::remove(temporary);
        throw;
    }
}
namespace {
const std::set<std::string>& bindingNames() {
    static const std::set<std::string> names{"Model.hpp", "model.json", "ModelView.hpp", "view.json", "bindings.json", "style.json"};
    return names;
}
void ownedPreflight(const fs::path& output, const std::set<std::string>& names) {
    require(output.is_absolute() && !symbolicPath(output), "output must be an absolute nonsymbolic directory");
    require(fs::weakly_canonical(output) == output.lexically_normal(), "output has symbolic ancestors");
    if (!fs::exists(output))
        return;
    require(fs::is_directory(output), "output must be a directory");
    for (const auto& entry : fs::directory_iterator(output)) {
        require(names.contains(entry.path().filename().string()) && fs::is_regular_file(entry.symlink_status()),
                "output directory contains unowned material");
    }
}
}
void outputPreflight(const fs::path& output) {
    ownedPreflight(output, bindingNames());
}
void publishOwnedDirectory(const fs::path& output, const std::map<std::string, std::string>& files,
                           const std::set<std::string>& ownedNames) {
    require(!files.empty() && !ownedNames.empty(), "publication requires explicit owned filenames");
    for (const auto& [name, bytes] : files)
        require(ownedNames.contains(name), "publication contains an unowned filename");
    ownedPreflight(output, ownedNames);
    fs::create_directories(output.parent_path());
    const auto temporary = output.parent_path() / (".pipelang-bindings-" + uniqueName());
    const auto stage = temporary / "bindings";
    const auto previous = temporary / "previous";
    fs::create_directories(stage);
    try {
        for (const auto& [name, bytes] : files) {
            portablePath(name);
            require(fs::path(name).filename() == name, "generated output must be a plain filename");
            atomicWrite(stage / name, bytes);
        }
        ownedPreflight(output, ownedNames);
        const bool existed = fs::exists(output);
        if (existed)
            fs::rename(output, previous);
        try {
            fs::rename(stage, output);
        } catch (...) {
            if (existed)
                fs::rename(previous, output);
            throw;
        }
        fs::remove_all(temporary);
    } catch (...) {
        // Never remove a backup if publication and its rollback both failed.
        if (!fs::exists(previous))
            fs::remove_all(temporary);
        throw;
    }
}
void publishDirectory(const fs::path& output, const std::map<std::string, std::string>& files) {
    publishOwnedDirectory(output, files, bindingNames());
}
}
