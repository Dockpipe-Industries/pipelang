// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "composition.hpp"

#include <cctype>

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

nlohmann::json compileProgram(Program program, const std::string& nameSpace,
                              const std::string& protocol,
                              const std::vector<std::string>& models = {}) {
    program.objectEnabled = protocol == "pipelang.native-model.v3";
    program.modelExports = models;
    normalizeComposition(program);
    check(program);
    return {{"protocol", protocol}, {"ok", true}, {"header", generate(program, nameSpace)},
            {"manifest", manifest(program, nameSpace)}};
}

}

nlohmann::json compile(const std::string& source, const std::string& nameSpace) {
    validateNamespace(nameSpace);
    auto program = parse(source);
    if (!program.module.empty() || !program.imports.empty())
        throw Diagnostic("module and import declarations require pipelang.native-model.v2", 0);
    return compileProgram(std::move(program), nameSpace, "pipelang.native-model.v1");
}

nlohmann::json compileModules(const std::vector<ModuleSource>& modules, const std::string& entry,
                              const std::string& nameSpace) {
    validateNamespace(nameSpace);
    return compileProgram(linkModules(modules, entry), nameSpace, "pipelang.native-model.v2");
}

nlohmann::json compileObjects(const std::string& source, const std::vector<std::string>& models,
                              const std::string& nameSpace) {
    validateNamespace(nameSpace);
    auto program = parse(source, true);
    if (!program.module.empty() || !program.imports.empty())
        throw Diagnostic("v3 module/import declarations require a closed modules request", 0);
    return compileProgram(std::move(program), nameSpace, "pipelang.native-model.v3", models);
}

nlohmann::json compileModuleObjects(const std::vector<ModuleSource>& modules, const std::string& entry,
                                    const std::vector<std::string>& models, const std::string& nameSpace) {
    validateNamespace(nameSpace);
    return compileProgram(linkModules(modules, entry, true), nameSpace, "pipelang.native-model.v3", models);
}

}
