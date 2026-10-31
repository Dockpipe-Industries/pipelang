// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <algorithm>
#include <array>
#include <regex>
#include <fstream>
#include <sstream>

namespace pipelang::tools {
Json auditExecutions(const fs::path& trace) {
    const auto bytes = readFile(trace, 16U << 20);
    const std::regex execution("execve\\(\"([^\"]+)\"");
    const std::regex forbidden("(go|gofmt|python[0-9.]*)(\\.exe)?");
    std::set<std::string> executables;
    for (auto match = std::sregex_iterator(bytes.begin(), bytes.end(), execution); match != std::sregex_iterator(); ++match) {
        const fs::path path = (*match)[1].str();
        require(!std::regex_match(path.filename().string(), forbidden) && path.extension() != ".py",
                "required native path attempted a Go or Python executable: " + path.string());
        std::ifstream input(path, std::ios::binary);
        std::array<char, 256> prefix{};
        input.read(prefix.data(), prefix.size());
        const std::string start(prefix.data(), static_cast<std::size_t>(input.gcount()));
        if (start.starts_with("#!")) {
            const auto interpreter = start.substr(0, start.find('\n'));
            require(interpreter.find("python") == std::string::npos && interpreter.find("gofmt") == std::string::npos,
                    "required native path invoked a forbidden script interpreter");
        }
        executables.insert(path.generic_string());
    }
    require(!executables.empty(), "execution trace contains no executable identities");
    return {{"protocol", "pipelang.native-execution-audit.v1"}, {"status", "passed"},
            {"trace_sha256", sha256(bytes)}, {"executables", executables}, {"go_or_python_attempted", false}};
}

Json dependencyInventory(const fs::path& root) {
    require(root.is_absolute() && fs::is_directory(root / "src/native"), "inventory requires the owning source root");
    Json forbidden = Json::array();
    std::size_t visited = 0;
    for (auto iterator = fs::recursive_directory_iterator(root); iterator != fs::recursive_directory_iterator(); ++iterator) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && (name == ".git" || name == "node_modules")) {
            iterator.disable_recursion_pending();
            continue;
        }
        require(++visited <= 200000, "repository inventory capacity exceeded");
        const auto extension = entry.path().extension();
        if (extension == ".go" || extension == ".py" || extension == ".pyc" || extension == ".pyo" ||
            name == "go.mod" || name == "go.sum" || name == "go.work" || name == "go.work.sum" ||
            name == "pyproject.toml" || name == "Pipfile" || name == "Pipfile.lock" || name == "poetry.lock" ||
            name == "requirements.txt" || name == "__pycache__")
            forbidden.push_back(entry.path().lexically_relative(root).generic_string());
    }
    require(forbidden.empty(), "retired language files remain: " + forbidden.dump());
    return {{"protocol", "pipelang.native-inventory.v2"}, {"status", "passed"}, {"go_python_files", 0},
        {"entries_checked", visited}, {"active_compilers", {"pipelang", "pipelang-native-cpp", "pipelang-modelc", "pipelang-model-viewc"}},
        {"retired", "Unused historical Go profiles, APIs, research harnesses and Python orchestration"}};
}
}
