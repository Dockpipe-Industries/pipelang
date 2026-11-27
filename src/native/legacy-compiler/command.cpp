// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <array>
#include <iostream>

namespace pipelang::legacy {
namespace {
struct Options {
    std::string operation;
    std::string profile;
    std::string input;
    std::string output = "bin/.dockpipe/pipelang";
    std::string owner;
    std::string method;
    std::string format = "text";
    std::vector<std::string> values;
    std::vector<fs::path> roots;
    fs::path workdir;
    bool force = false;
    bool standardInput = false;
};
Options options(const std::vector<std::string>& arguments) {
    require(!arguments.empty(), "missing check, compile or invoke command");
    Options result;
    result.operation = arguments.front();
    require(result.operation == "check" || result.operation == "compile" || result.operation == "invoke" || result.operation == "materialize", "unknown native compiler command");
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (name == "--force" && result.operation == "materialize") {
            result.force = true;
            continue;
        }
        if (name == "--stdin" && result.operation == "check") {
            require(!result.standardInput, "duplicate --stdin");
            result.standardInput = true;
            continue;
        }
        if (!name.starts_with('-') && result.input.empty()) {
            result.input = name;
            continue;
        }
        const bool shared = name == "--profile" || name == "--in";
        const bool compile = result.operation == "compile" && (name == "--out" || name == "--entry");
        const bool invoke = result.operation == "invoke" && (name == "--class" || name == "--method" || name == "--arg");
        const bool format = (result.operation == "check" || result.operation == "invoke") && name == "--format";
        const bool materialize = result.operation == "materialize" && (name == "--workdir" || name == "--from");
        require(shared || compile || invoke || format || materialize, "unknown option " + name);
        require(++index < arguments.size(), name + " requires a value");
        const auto& value = arguments[index];
        if (name == "--profile")
            result.profile = value;
        else if (name == "--in")
            result.input = value;
        else if (name == "--out")
            result.output = value;
        else if (name == "--entry" || name == "--class")
            result.owner = value;
        else if (name == "--method")
            result.method = value;
        else if (name == "--arg")
            result.values.push_back(value);
        else if (name == "--from")
            result.roots.emplace_back(value);
        else if (name == "--workdir")
            result.workdir = value;
        else
            result.format = value;
    }
    require(result.profile.empty() || result.profile == "v0.0.0.1", "unsupported retired configuration profile");
    require(result.operation == "materialize" || !result.input.empty(), "missing input file (use --in <file.pipe>)");
    require(result.operation != "materialize" || (result.input.empty() && result.format == "text"), "materialize does not accept input or format options");
    require(result.format == "text" || result.format == "json" || (result.operation == "invoke" && result.format == "env"), "invalid output format");
    require(result.operation != "invoke" || !result.method.empty(), "--method is required");
    return result;
}
std::string stdinBytes(std::istream& input) {
    std::string result;
    std::array<char, 4096> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        require(static_cast<std::size_t>(input.gcount()) <= 65536 - result.size(), "stdin exceeds its byte limit");
        result.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    require(input.eof(), "failed to read stdin");
    return result;
}
std::string quote(const std::string& value) {
    std::string result = "'";
    for (char character : value)
        result += character == '\'' ? "'\"'\"'" : std::string(1, character);
    return result + '\'';
}
}
void publishArtifacts(const fs::path& directory, const std::string& base, const Artifacts& artifacts) {
    portablePath(base);
    require(fs::path(base).filename() == base, "artifact base must be a plain filename");
    require(fs::weakly_canonical(directory) == directory, "output directory has symbolic ancestors");
    fs::create_directories(directory);
    const std::map<std::string, std::string> files{
        {base + ".workflow.yml", artifacts.workflow},
        {base + ".bindings.json", artifacts.bindings},
        {base + ".bindings.env", artifacts.environment}};
    for (const auto& [name, bytes] : files) {
        const auto path = directory / name;
        if (fs::exists(fs::symlink_status(path)))
            regularFile(path);
    }
    const auto temporary = directory / (".pipelang-publish-" + uniqueName());
    fs::create_directories(temporary / "new");
    fs::create_directories(temporary / "old");
    std::vector<std::string> replaced;
    std::vector<std::string> created;
    try {
        for (const auto& [name, bytes] : files)
            atomicWrite(temporary / "new" / name, bytes);
        for (const auto& [name, bytes] : files) {
            const auto destination = directory / name;
            if (fs::exists(fs::symlink_status(destination))) {
                regularFile(destination);
                fs::rename(destination, temporary / "old" / name);
                replaced.push_back(name);
            }
            fs::rename(temporary / "new" / name, destination);
            created.push_back(name);
        }
    } catch (...) {
        // Keep recovery material if rollback itself fails.
        for (const auto& name : created)
            fs::remove(directory / name);
        for (const auto& name : replaced)
            fs::rename(temporary / "old" / name, directory / name);
        fs::remove_all(temporary);
        throw;
    }
    fs::remove_all(temporary);
}
int command(const std::vector<std::string>& arguments, std::istream& input, std::ostream& output) {
    if (arguments.empty() || arguments == std::vector<std::string>{"--help"}) {
        output << "pipelang check|compile|invoke --in FILE [--profile v0.0.0.1] [options]\n"
                  "compile: [--entry CLASS] [--out DIRECTORY]\n"
                  "invoke: [--class CLASS] --method METHOD [--arg VALUE]... [--format text|json|env]\n"
                  "check: [--stdin] [--format text|json]\n"
                  "materialize [--profile v0.0.0.1] [--workdir PROJECT] [--from ROOT]... [--force]\n";
        return 0;
    }
    const auto selected = options(arguments);
    if (selected.operation == "materialize") {
        const auto project = fs::absolute(selected.workdir.empty() ? fs::current_path() : selected.workdir).lexically_normal();
        const auto destination = project / "bin/.dockpipe/pipelang";
        std::map<std::string, std::string> ids{{"project", project.filename().string()}, {"workdir", project.generic_string()},
            {"output_root", destination.generic_string()}, {"force", selected.force ? "true" : "false"}};
        operation("pipelang.materialize", ids, [&] {
            const auto roots = existingRoots(selected.roots.empty() ? workflowRoots(project) : selected.roots);
            ids["root_count"] = std::to_string(roots.size());
            ids["artifact_count"] = std::to_string(materialize(roots, selected.force, destination));
            ids["result"] = "materialized";
        });
        return 0;
    }
    if (selected.operation == "compile") {
        const auto destination = fs::path(selected.output).lexically_normal();
        std::map<std::string, std::string> ids{{"input_path", fs::path(selected.input).lexically_normal().generic_string()},
            {"output_dir", destination.generic_string()}};
        operation("pipelang.compile", ids, [&] {
            const auto path = fs::absolute(selected.input).lexically_normal();
            const auto sources = readSources(moduleRoot(path.parent_path()));
            require(sources.contains(path.generic_string()), "input is not an admitted .pipe source");
            auto owner = selected.owner;
            if (owner.empty()) {
                const auto local = parse(sources.at(path.generic_string()), path.generic_string());
                for (const auto& declaration : local.declarations) {
                    if (!declaration.contract) {
                        owner = declaration.name;
                        break;
                    }
                }
            }
            if (!owner.empty())
                ids["entry_class"] = owner;
            const auto artifacts = compile(merge(sources), owner);
            ids["entry_class"] = artifacts.entry;
            ids["workflow_path"] = (destination / (artifacts.entry + ".workflow.yml")).generic_string();
            ids["bindings_json_path"] = (destination / (artifacts.entry + ".bindings.json")).generic_string();
            ids["bindings_env_path"] = (destination / (artifacts.entry + ".bindings.env")).generic_string();
            publishArtifacts(fs::absolute(destination).lexically_normal(), artifacts.entry, artifacts);
            ids["result"] = "compiled";
        });
        return 0;
    }
    const auto path = fs::absolute(selected.input).lexically_normal();
    auto sources = readSources(moduleRoot(path.parent_path()));
    if (selected.standardInput)
        sources[path.generic_string()] = stdinBytes(input);
    require(sources.contains(path.generic_string()), "input is not an admitted .pipe source");
    if (selected.operation == "check") {
        const auto diagnostics = analyze(sources);
        if (selected.format == "json")
            output << diagnosticsJson(sources, diagnostics);
        else {
            for (const auto& diagnostic : diagnostics)
                output << diagnosticText(sources, diagnostic) << '\n';
        }
        return diagnostics.empty() ? 0 : 1;
    }
    const auto program = merge(sources);
    check(program);
    auto owner = selected.owner;
    if (owner.empty()) {
        const auto entryProgram = parse(sources.at(path.generic_string()), path.generic_string());
        for (const auto& declaration : entryProgram.declarations) {
            if (!declaration.contract) {
                owner = declaration.name;
                break;
            }
        }
    }
    const auto value = invoke(program, owner, selected.method, selected.values);
    if (owner.empty()) {
        for (const auto& declaration : program.declarations) {
            if (!declaration.contract && declaration.visible) {
                owner = declaration.name;
                break;
            }
        }
    }
    std::string resultType;
    for (const auto& declaration : program.declarations) {
        if (declaration.name != owner || declaration.contract)
            continue;
        for (const auto& method : declaration.methods) {
            if (method.name == selected.method)
                resultType = method.result;
        }
    }
    if (selected.format == "json")
        output << resultJson({{"class", owner}, {"method", selected.method}, {"type", resultType}, {"value", value.data}}) << '\n';
    else if (selected.format == "env")
        output << "PIPELANG_RESULT_TYPE=" << resultType << "\nPIPELANG_RESULT=" << quote(value.string()) << '\n';
    else
        output << value.string() << '\n';
    return 0;
}
}
