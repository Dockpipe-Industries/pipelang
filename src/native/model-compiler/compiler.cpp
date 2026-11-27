// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "composition.hpp"
#include "CheckedProgram.hpp"
#include "OutputBudgetStream.hpp"
#include "CompositionPlanner.hpp"

#include <cctype>
#include <ostream>

namespace pipelang::models {
namespace {

void validateNamespace(const std::string& nameSpace) {
    if (nameSpace.empty() || nameSpace.size() > 96 || nameSpace.starts_with('_'))
        throw Diagnostic("invalid generated namespace", 0);
    for (std::size_t index = 0; index < nameSpace.size(); ++index) {
        const auto character = static_cast<unsigned char>(nameSpace[index]);
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              character == '_' || (index > 0 && character >= '0' && character <= '9')))
            throw Diagnostic("invalid generated namespace", 0);
    }
    // Reuse identifier admission, including native keyword reservations.
    parse("public Record " + nameSpace + " { }");
}

nlohmann::json compileProgram(const CheckedProgram& checked, const std::string& nameSpace,
                              const std::string& protocol,
                              const std::string& emission = {},
                              bool emitCompositionFactory = false) {
    const auto& program = checked.definition();
    const auto plans = compositionPlans(program);
    if (emitCompositionFactory && program.compositions.empty())
        throw Diagnostic("composition factory emission requires a source composition declaration", 0);
    nlohmann::json response;
    if (emission.empty()) {
        response = {{"protocol", protocol}, {"ok", true}, {"header", generate(program, nameSpace)},
                    {"manifest", manifest(program, nameSpace)}};
    } else {
        const auto generated = generateCompiledProject(program, nameSpace);
        response = {{"protocol", protocol}, {"ok", true}, {"header", generated.header},
                    {"manifest", manifest(program, nameSpace)}};
        auto files = nlohmann::json::array();
        for (const auto& [name, content] : generated.files) {
            nlohmann::json file{{"name", name}, {"kind", name == "Objects.hpp" ? "private-header" : "implementation"}};
            if (emission == "sources")
                file["content"] = content;
            files.push_back(std::move(file));
        }
        response["implementation"] = {{"protocol", "pipelang.native-project-implementation.v1"},
                                       {"emission", emission}, {"files", std::move(files)}};
    }
    if (program.projectEnabled) {
        response["types_header"] = generateProjectTypes(program, nameSpace);
        response["numeric_header"] = generateNumericHeader(program, nameSpace);
    }
    if (!constantsManifest(program, nameSpace).empty())
        response["constants_header"] = constantsHeader(program, nameSpace);
    if (!program.compositions.empty())
        response["composition_plans"] = plans;
    if (emitCompositionFactory)
        response["composition_factory_header"] = generateCompositionFactory(program, nameSpace, !emission.empty());
    // Include the CLI newline in the common direct/API output budget.
    OutputBudgetStream counter(resourceBudget().outputBytes - 1);
    std::ostream output(&counter);
    output << response;
    if (!output || counter.exceeded())
        throw Diagnostic("output_bytes resource budget exceeded; configure output_bytes", 0);
    return response;
}

}

nlohmann::json compile(const std::string& source, const std::string& nameSpace) {
    validateNamespace(nameSpace);
    auto program = parse(source);
    if (!program.module.empty() || !program.imports.empty())
        throw Diagnostic("module and import declarations require pipelang.native-model.v2", 0);
    return compileProgram(CheckedProgram::complete(std::move(program)), nameSpace, "pipelang.native-model.v1");
}

nlohmann::json compileModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                              const std::string& nameSpace) {
    validateNamespace(nameSpace);
    return compileProgram(CheckedProgram::complete(linkModules(modules, entry)), nameSpace, "pipelang.native-model.v2");
}

nlohmann::json compileObjects(const std::string& source, const std::vector<std::string>& models,
                              const std::string& nameSpace) {
    validateNamespace(nameSpace);
    auto program = parse(source, true);
    if (!program.module.empty() || !program.imports.empty())
        throw Diagnostic("v3 module/import declarations require a closed modules request", 0);
    return compileProgram(CheckedProgram::complete(std::move(program), true, models),
                          nameSpace, "pipelang.native-model.v3");
}

nlohmann::json compileModuleObjects(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::vector<std::string>& models, const std::string& nameSpace) {
    validateNamespace(nameSpace);
    return compileProgram(CheckedProgram::complete(linkModules(modules, entry, true), true, models),
                          nameSpace, "pipelang.native-model.v3");
}

nlohmann::json compileProjectModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::string& nameSpace, bool activeHostNumericEnvironment) {
    validateNamespace(nameSpace);
    return compileProgram(CheckedProgram::project(modules, entry, activeHostNumericEnvironment),
                          nameSpace, "pipelang.native-model-project.v1");
}

nlohmann::json compileProjectImplementation(const std::vector<ModuleSource>& modules,
                                           const std::string& entry, const std::string& nameSpace,
                                           const std::string& emission, bool activeHostNumericEnvironment) {
    validateNamespace(nameSpace);
    if (emission != "plan" && emission != "sources")
        throw Diagnostic("compiled project emission must be plan or sources", 0);
    return compileProgram(CheckedProgram::project(modules, entry, activeHostNumericEnvironment),
                          nameSpace, "pipelang.native-model-project.v2", emission);
}

nlohmann::json compileProjectCompositionFactory(const std::vector<ModuleSource>& modules,
                                                const std::string& entry, const std::string& nameSpace,
                                                const std::string& emission, bool activeHostNumericEnvironment) {
    validateNamespace(nameSpace);
    if (!emission.empty() && emission != "plan" && emission != "sources")
        throw Diagnostic("compiled composition emission must be plan or sources", 0);
    const std::string protocol = emission.empty()
        ? "pipelang.native-model-project.v1" : "pipelang.native-model-project.v2";
    return compileProgram(CheckedProgram::project(modules, entry, activeHostNumericEnvironment),
                          nameSpace, protocol, emission, true);
}

}
