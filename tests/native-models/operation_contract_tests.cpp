// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/support/support.hpp"
#include <iostream>
#include <regex>
#include <sstream>

namespace {
using namespace pipelang::tools;
Json events(const fs::path& path) {
    std::istringstream lines(readFile(path, 1U << 20));
    std::string line;
    Json result = Json::array();
    while (std::getline(lines, line))
        result.push_back(parseJson(line, 65536));
    return result;
}
void eventContract(const Json& event, const std::string& unit, const std::string& status) {
    exactObject(event, {"schema", "type", "ts", "unit", "status", "ids"}, {"error"}, "operation event");
    require(event.at("schema") == "dockpipe.operation_event.v1" && event.at("type") == "operation_result" &&
        event.at("unit") == unit && event.at("status") == status, "operation identity/status changed");
    const std::regex timestamp("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,9})?Z");
    require(std::regex_match(event.at("ts").get<std::string>(), timestamp), "operation timestamp is not RFC3339Nano UTC");
}
void test(const fs::path& output, const fs::path& executable) {
    const auto project = output / "project";
    const auto source = project / "workflows/config.pipe";
    const auto root = source.parent_path();
    const auto log = output / "events.jsonl";
    const auto generated = output / "generated<&>";
    atomicWrite(root / "config.yml", "");
    atomicWrite(source, "Class Demo { string IMAGE = \"nginx\"; int Add(int x) => x + 1; }");
    const auto invoke = [&](std::vector<std::string> arguments, const fs::path& selectedLog) {
        std::vector<std::string> command{"/usr/bin/env", "DOCKPIPE_EVENT_LOG=" + selectedLog.string(), executable.string()};
        command.insert(command.end(), arguments.begin(), arguments.end());
        return process(command);
    };
    const auto compiled = invoke({"compile", "--profile", "v0.0.0.1", "--in", source.string(), "--out", generated.string()}, log);
    require(compiled.exit == 0 && compiled.output.empty() && compiled.diagnostic == "pipelang.compile: done\n", "compile operation summary differs");
    auto recorded = events(log);
    require(recorded.size() == 1, "compile must emit exactly one event");
    eventContract(recorded[0], "pipelang.compile", "done");
    require(recorded[0].at("ids") == Json({{"input_path", source.generic_string()}, {"output_dir", generated.generic_string()},
        {"entry_class", "Demo"}, {"workflow_path", (generated / "Demo.workflow.yml").generic_string()},
        {"bindings_json_path", (generated / "Demo.bindings.json").generic_string()},
        {"bindings_env_path", (generated / "Demo.bindings.env").generic_string()}, {"result", "compiled"}}), "compile operation IDs differ");
    require(readFile(log, 65536).find("\\u003c") != std::string::npos, "operation JSON lost HTML escaping");
    require((fs::status(log).permissions() & fs::perms::mask) == (fs::perms::owner_read | fs::perms::owner_write), "new operation log is not private");
    const auto missing = root / "missing.pipe";
    require(invoke({"compile", "--profile", "v0.0.0.1", "--in", missing.string(), "--out", generated.string()}, log).exit != 0, "missing source compile succeeded");
    recorded = events(log);
    require(recorded.size() == 2, "failed compile did not append exactly one event");
    eventContract(recorded[1], "pipelang.compile", "fail");
    require(recorded[1].at("ids").at("input_path") == missing.generic_string() && recorded[1].at("ids").at("result") == "failed" &&
        !recorded[1].at("error").get<std::string>().empty(), "failed compile lost identity/error");
    const auto materialized = invoke({"materialize", "--profile", "v0.0.0.1", "--workdir", project.string(), "--from", root.string(), "--from", root.string(), "--force"}, log);
    require(materialized.exit == 0 && materialized.diagnostic == "pipelang.materialize: done\n", "materializer operation summary differs");
    recorded = events(log);
    require(recorded.size() == 3, "materializer did not append exactly one event");
    eventContract(recorded[2], "pipelang.materialize", "done");
    require(recorded[2].at("ids") == Json({{"project", project.filename().string()}, {"workdir", project.generic_string()},
        {"output_root", (project / "bin/.dockpipe/pipelang").generic_string()}, {"force", "true"},
        {"root_count", "1"}, {"artifact_count", "1"}, {"result", "materialized"}}), "materializer operation IDs differ");
    require(invoke({"check", "--profile", "v0.0.0.1", "--in", source.string()}, log).exit == 0, "inert check failed");
    require(invoke({"invoke", "--profile", "v0.0.0.1", "--in", source.string(), "--method", "Add", "--arg", "1"}, log).output == "2\n", "pure invocation failed");
    require(events(log).size() == 3, "check/invoke unexpectedly emitted operation events");
    atomicWrite(root / "config.yml", "types: [\n");
    require(invoke({"materialize", "--profile", "v0.0.0.1", "--workdir", project.string()}, log).exit != 0, "malformed materialization succeeded");
    recorded = events(log);
    eventContract(recorded.back(), "pipelang.materialize", "fail");
    require(recorded.back().at("ids").at("result") == "failed" && !recorded.back().at("error").get<std::string>().empty(), "materializer failure event lost error/result");
    const auto directoryLog = output / "not-a-log";
    fs::create_directory(directoryLog);
    const auto failedLog = invoke({"compile", "--profile", "v0.0.0.1", "--in", source.string(), "--out", (output / "log-failure-artifacts").string()}, directoryLog);
    require(failedLog.exit != 0 && failedLog.diagnostic.find("record pipelang.compile event:") != std::string::npos &&
        fs::exists(output / "log-failure-artifacts/Demo.workflow.yml"), "operation log failure did not propagate after successful work");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"contracts", {"compile-summary-and-ids", "failure-event",
        "materialize-summary-and-ids", "append-jsonl", "rfc3339nano", "html-escaping", "private-log", "inert-check-invoke", "materialize-failure", "log-error-propagation"}}}));
}
}
int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc == 3, "operation tests require fresh proof and installed native CLI");
        const fs::path output = argv[1];
        require(output.is_absolute() && !fs::exists(output), "operation proof must be fresh and absolute");
        fs::create_directories(output);
        test(output, argv[2]);
        std::cout << "10 native operation event contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "operation oracle: " << error.what() << '\n';
        return 1;
    }
}
