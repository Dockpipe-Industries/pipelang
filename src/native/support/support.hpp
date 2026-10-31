// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipelang::tools {
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string sha256(std::string_view bytes);
std::string fileHash(const fs::path& path);
std::string readFile(const fs::path& path, std::size_t maximum);
Json parseJson(std::string_view bytes, std::size_t maximum = 16U << 20);
std::string canonical(const Json& value, bool ascii = false);
void exactObject(const Json& value, const std::set<std::string>& required,
                 const std::set<std::string>& optional, const std::string& label);
std::string text(const Json& value, const std::string& label, std::size_t maximum = 65536);
void require(bool condition, const std::string& message);
Json dependencyInventory(const fs::path& root);
Json auditExecutions(const fs::path& trace);
void portablePath(const std::string& value);
void regularFile(const fs::path& path);
void atomicWrite(const fs::path& path, std::string_view bytes);
void publishDirectory(const fs::path& output, const std::map<std::string, std::string>& files);
void publishOwnedDirectory(const fs::path& output, const std::map<std::string, std::string>& files,
                           const std::set<std::string>& ownedNames);
fs::path executablePath();
void outputPreflight(const fs::path& output);
std::string uniqueName();

struct ProcessResult {
    int exit = -1;
    std::string output;
    std::string diagnostic;
};

ProcessResult process(const std::vector<std::string>& arguments, std::string_view input = {},
                      std::chrono::milliseconds timeout = std::chrono::seconds(45),
                      std::size_t maximumOutput = 1U << 20, std::size_t maximumDiagnostic = 65536,
                      const std::function<void()>& monitor = {}, const fs::path& log = {});

class Arguments {
public:
    Arguments(int argc, char** argv, int first = 1);
    std::string take(const std::string& name);
    std::string optional(const std::string& name, const std::string& fallback = {});
    bool flag(const std::string& name);
    std::vector<std::string> rest();
    void finish() const;
private:
    std::vector<std::string> values_;
};
}
