// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <iostream>
#include <regex>

namespace {
using namespace pipelang::tools;
void successful(const std::vector<std::string>& command) {
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "repository check command failed: " + result.output + result.diagnostic);
}
void actions(const YAML::Node& node, unsigned depth, std::size_t& visited) {
    require(depth <= 64 && ++visited <= 20000, "CI YAML traversal bound exceeded");
    if (node.IsSequence()) {
        for (const auto& child : node)
            actions(child, depth + 1, visited);
    } else if (node.IsMap()) {
        for (const auto& entry : node) {
            require(entry.first.IsScalar(), "CI mapping has a nonscalar key");
            if (entry.first.Scalar() == "uses") {
                require(entry.second.IsScalar(), "CI action reference must be scalar");
                const auto reference = entry.second.Scalar();
                if (!reference.starts_with("./") && !reference.starts_with("docker://"))
                    require(std::regex_match(reference, std::regex("[^@]+@[0-9a-f]{40}")), "remote action is not pinned to a full commit: " + reference);
                require(!reference.starts_with("actions/setup-go@") && !reference.starts_with("actions/setup-python@"), "retired CI toolchain returned");
            }
            actions(entry.second, depth + 1, visited);
        }
    }
}
void pins(const fs::path& root, const fs::path& dockpipe) {
    const auto config = parseJson(readFile(root / "dockpipe.config.json", 1U << 20));
    const auto lock = parseJson(readFile(root / "dockpipe.lock.json", 1U << 20));
    const auto version = process({dockpipe.string(), "--version"}, {}, std::chrono::seconds(15));
    require(version.exit == 0 && version.diagnostic.empty(), "cannot identify the installed DockPipe");
    const auto expected = text(config.at("dockpipe_version"), "pinned DockPipe version");
    require(version.output == expected + '\n' && lock.at("dockpipe").at("version") == expected, "DockPipe executable/config/lock versions disagree");
    require(config.at("schema") == 1 && lock.at("schema") == 1 && !config.contains("min_dockpipe_version"), "unadmitted toolchain configuration");
    const auto& artifacts = lock.at("dockpipe").at("artifacts");
    const std::set<std::string> targets{"linux/amd64", "linux/arm64", "darwin/amd64", "darwin/arm64", "windows/amd64"};
    require(artifacts.is_object() && artifacts.size() == targets.size(), "DockPipe lock must cover all five targets");
    const auto release = text(lock.at("dockpipe").at("release"), "locked release");
    for (const auto& target : targets) {
        const auto& artifact = artifacts.at(target);
        const auto url = text(artifact.at("url"), "locked artifact URL");
        require(url.starts_with("https://github.com/Dockpipe-Industries/dockpipe/releases/download/" + release + '/'), "unofficial locked DockPipe download");
        require(std::regex_match(text(artifact.at("sha256"), "artifact hash"), std::regex("[0-9a-f]{64}")), "malformed DockPipe artifact checksum");
        require(artifact.at("size_bytes").is_number_unsigned() && artifact.at("size_bytes").get<std::uint64_t>() > 0 &&
            artifact.at("size_bytes").get<std::uint64_t>() <= 128U << 20, "locked archive size is not bounded");
        require(artifact.at("executable_size_bytes").is_number_unsigned() && artifact.at("executable_size_bytes").get<std::uint64_t>() > 0 &&
            artifact.at("executable_size_bytes").get<std::uint64_t>() <= 64U << 20, "locked executable size is not bounded");
        require(artifact.at("executable") == (target.starts_with("windows/") ? "dockpipe.exe" : "dockpipe"), "locked executable name differs");
    }
    require(!fs::exists(root / "tooling/dockpipe-bootstrap-pending.json"), "obsolete bootstrap pending marker returned");
}
void nativeOnly(const fs::path& root) {
    std::size_t visited = 0;
    for (auto iterator = fs::recursive_directory_iterator(root); iterator != fs::recursive_directory_iterator(); ++iterator) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && (name == ".git" || name == "node_modules")) {
            iterator.disable_recursion_pending();
            continue;
        }
        require(++visited <= 200000, "repository traversal capacity exceeded");
        const auto extension = entry.path().extension().string();
        require(extension != ".go" && extension != ".py" && extension != ".pyc" && extension != ".pyo" &&
            name != "go.mod" && name != "go.sum" && name != "go.work" && name != "go.work.sum" &&
            name != "pyproject.toml" && name != "requirements.txt" && name != "Pipfile" && name != "poetry.lock" && name != "__pycache__",
            "retired language file returned: " + entry.path().string());
        if (entry.is_regular_file() && (extension == ".sh" || extension == ".yml" || extension == ".cmake" || name == "CMakeLists.txt")) {
            const auto bytes = readFile(entry.path(), 1U << 20);
            require(!std::regex_search(bytes, std::regex(R"((^|[ \t/;])(python[0-9.]*|gofmt)([ \t]|$)|go (build|test|run|install|mod|generate) )")),
                "retired language command returned: " + entry.path().string());
        }
    }
}

}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        Arguments arguments(argc, argv);
        const fs::path root = arguments.take("--source-root");
        const fs::path dockpipe = arguments.take("--dockpipe");
        const fs::path output = arguments.take("--output");
        arguments.finish();
        require(root.is_absolute() && dockpipe.is_absolute() && output.is_absolute() && !fs::exists(output), "repository check requires absolute tools/root and fresh proof");
        fs::create_directories(output);
        pins(root, dockpipe);
        Json checks = Json::array({"exact-toolchain-pins"});
        nativeOnly(root);
        checks.push_back("zero-Go-Python-files-and-toolchains");
        std::size_t workflows = 0;
        for (const auto& directory : fs::directory_iterator(root / "workflows")) {
            const auto config = directory.path() / "config.yml";
            if (directory.is_directory() && fs::exists(config)) {
                successful({dockpipe.string(), "workflow", "validate", config.string()});
                ++workflows;
            }
        }
        checks.push_back("declared-workflow-validation");
        std::size_t scripts = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root / "workflows")) {
            if (entry.path().extension() == ".sh") {
                successful({"/usr/bin/bash", "-n", entry.path().string()});
                ++scripts;
            }
        }
        checks.push_back("shell-syntax");
        for (const auto& entry : fs::directory_iterator(root / ".github/workflows")) {
            if (entry.path().extension() == ".yml") {
                std::size_t visited = 0;
                actions(YAML::Load(readFile(entry.path(), 1U << 20)), 0, visited);
            }
        }
        checks.push_back("full-action-commit-pins");
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"checks", checks}, {"workflow_count", workflows},
            {"shell_scripts", scripts}, {"go_python_files", 0},
            {"scope", "native workflow, exact pin, source and orchestration hygiene"}}));
        std::cout << "C++ repository checks passed; zero Go/Python files or workflow toolchains\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native repository check: " << error.what() << '\n';
        return 1;
    }
}
