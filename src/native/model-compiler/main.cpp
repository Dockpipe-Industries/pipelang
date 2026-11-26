// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"
#include "ScopedResourceBudget.hpp"
#include "../lexer/lexer.hpp"

#include <array>
#include <iostream>
#include <set>

namespace {

using nlohmann::json;

void requireFields(const json& value, const std::set<std::string>& required,
                   const std::set<std::string>& optional = {}) {
    if (!value.is_object())
        throw std::runtime_error("request value must be an object");
    for (const auto& name : required) {
        if (!value.contains(name))
            throw std::runtime_error("missing request field '" + name + "'");
    }
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (!required.contains(item.key()) && !optional.contains(item.key()))
            throw std::runtime_error("unknown request field '" + item.key() + "'");
    }
}

}

int main(int argc, char** argv) {
    try {
        pipelang::models::ResourceBudget configured;
        if (argc != 1) {
            if (argc != 3 || std::string(argv[1]) != "--resource-budget-json")
                throw std::runtime_error("usage: pipelang-modelc [--resource-budget-json object]");
            configured = pipelang::models::ResourceBudget::fromText(argv[2]);
        }
        const pipelang::models::ScopedResourceBudget budgetScope(configured);
        const auto& budget = pipelang::models::resourceBudget();
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (pipelang::models::budgetExceeded(input.size(), count, budget.inputBytes))
                throw std::runtime_error("request input_bytes resource budget exceeded; configure input_bytes");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("failed to read request");
        std::vector<std::set<std::string>> objectKeys;
        auto request = json::parse(input, [&](int depth, json::parse_event_t event, json& parsed) {
            if (depth > 16)
                throw std::runtime_error("request JSON nesting limit exceeded");
            if (event == json::parse_event_t::object_start)
                objectKeys.emplace_back();
            else if (event == json::parse_event_t::object_end)
                objectKeys.pop_back();
            else if (event == json::parse_event_t::key && !objectKeys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("duplicate request key");
            return true;
        });
        if (!request.is_object() || !request.contains("protocol") || !request["protocol"].is_string())
            throw std::runtime_error("invalid native model request");
        const auto protocol = request["protocol"].get<std::string>();
        const bool formatting = protocol == "pipelang.native-model-format.v1";
        const bool index = protocol == "pipelang.native-model-index.v1";
        const bool analysis = protocol == "pipelang.native-model-analysis.v1";
        const bool compiled = protocol == "pipelang.native-model-project.v2";
        const bool project = analysis || compiled || protocol == "pipelang.native-model-project.v1";
        const bool objects = protocol == "pipelang.native-model.v3";
        const bool modular = project || protocol == "pipelang.native-model.v2" || (objects && request.contains("modules"));
        if (protocol != "pipelang.native-model.v1" && protocol != "pipelang.native-model.v2" && !objects && !project && !index && !formatting)
            throw std::runtime_error("unsupported native model protocol");
        auto required = modular ? std::set<std::string>{"protocol", "modules", "entry"}
                                : std::set<std::string>{"protocol", "source"};
        if (index)
            required = {"protocol", "sources"};
        if (objects)
            required.insert("models");
        if (compiled)
            required.insert("emit");
        auto optional = std::set<std::string>{"namespace"};
        if (formatting) {
            optional = {"tab_size", "insert_spaces", "range_start", "range_end"};
        }
        if (project && !analysis) {
            optional.insert("native_numeric_environment");
            optional.insert("composition_emission");
        }
        requireFields(request, required, optional);
        const bool emitCompositionFactory = request.contains("composition_emission");
        if (emitCompositionFactory && request.at("composition_emission") != "factory")
            throw std::runtime_error("composition_emission must be factory");
        const bool activeHostNumericEnvironment = request.contains("native_numeric_environment");
        if (activeHostNumericEnvironment && request.at("native_numeric_environment") != "active-host")
            throw std::runtime_error("native_numeric_environment must be active-host");
        if (compiled && (!request.at("emit").is_string() ||
            (request.at("emit") != "plan" && request.at("emit") != "sources")))
            throw std::runtime_error("compiled project emit must be plan or sources");
        std::vector<std::string> models;
        if (objects) {
            if (!request["models"].is_array() || request["models"].empty() || request["models"].size() > budget.declarations)
                throw std::runtime_error("v3 requires 1..128 explicit model exports");
            std::set<std::string> names;
            for (const auto& model : request["models"]) {
                if (!model.is_string())
                    throw std::runtime_error("model export names must be strings");
                auto name = model.get<std::string>();
                if (name.empty() || name.size() > 1024 || !names.insert(name).second)
                    throw std::runtime_error("model export names must be nonempty, bounded and unique");
                models.push_back(std::move(name));
            }
        }
        if (request.contains("namespace") && !request["namespace"].is_string())
            throw std::runtime_error("namespace must be a string");
        std::vector<pipelang::models::ModuleSource> modules;
        if (index) {
            if (!request["sources"].is_array() || request["sources"].empty() ||
                request["sources"].size() > budget.sourceFragments)
                throw std::runtime_error("index requires 1..4096 source fragments");
            for (const auto& source : request["sources"]) {
                requireFields(source, {"path", "source"});
                if (!source["path"].is_string() || !source["source"].is_string())
                    throw std::runtime_error("index paths and sources must be strings");
                modules.push_back({{}, source["source"].get<std::string>(), source["path"].get<std::string>()});
            }
        } else if (modular) {
            if (!request["entry"].is_string() || !request["modules"].is_array() ||
                request["modules"].empty() || request["modules"].size() > (project ? budget.sourceFragments : budget.modules))
                throw std::runtime_error(project ? "project requires an entry and 1..4096 source fragments"
                                                 : "v2 requires an entry and 1..128 closed source modules");
            for (auto& module : request["modules"]) {
                requireFields(module, {"name", "source"}, project ? std::set<std::string>{"path"} : std::set<std::string>{});
                if (!module["name"].is_string() || !module["source"].is_string())
                    throw std::runtime_error("module name and source must be strings");
                if (module.contains("path") && !module["path"].is_string())
                    throw std::runtime_error("project fragment path must be a string");
                modules.push_back({module["name"].get<std::string>(), std::move(module["source"].get_ref<std::string&>()),
                                   project ? module.value("path", std::string{}) : std::string{}});
            }
        } else if (!request["source"].is_string()) {
            throw std::runtime_error("source must be a string");
        }
        json response;
        try {
            const auto nameSpace = request.value("namespace", "pipelang_models");
            if (formatting) {
                if (request.contains("tab_size") && (!request["tab_size"].is_number_integer() ||
                    request["tab_size"].get<std::int64_t>() < 1 || request["tab_size"].get<std::int64_t>() > 16))
                    throw std::runtime_error("format tab_size must be an integer in 1..16");
                if (request.contains("insert_spaces") && !request["insert_spaces"].is_boolean())
                    throw std::runtime_error("format insert_spaces must be boolean");
                for (const auto* field : {"range_start", "range_end"}) {
                    if (request.contains(field) && (!request[field].is_number_integer() ||
                        request[field].get<std::int64_t>() < 0))
                        throw std::runtime_error("format range offsets must be nonnegative integers");
                }
                const auto& source = request["source"].get_ref<const std::string&>();
                response = pipelang::models::formatSource(source, request.value("tab_size", 4U),
                    request.value("insert_spaces", true), request.value("range_start", std::size_t{0}),
                    request.value("range_end", source.size()));
            } else if (index) {
                response = pipelang::models::indexProjectSources(modules);
            } else if (analysis) {
                response = pipelang::models::analyzeProjectModules(modules, request["entry"].get<std::string>());
            } else if (emitCompositionFactory) {
                response = pipelang::models::compileProjectCompositionFactory(modules,
                    request["entry"].get<std::string>(), nameSpace,
                    compiled ? request.at("emit").get<std::string>() : std::string{}, activeHostNumericEnvironment);
            } else if (compiled) {
                response = pipelang::models::compileProjectImplementation(modules,
                    request["entry"].get<std::string>(), nameSpace, request.at("emit").get<std::string>(), activeHostNumericEnvironment);
            } else if (project) {
                response = pipelang::models::compileProjectModules(modules, request["entry"].get<std::string>(), nameSpace, activeHostNumericEnvironment);
            } else if (objects) {
                response = modular
                    ? pipelang::models::compileModuleObjects(modules, request["entry"].get<std::string>(), models, nameSpace)
                    : pipelang::models::compileObjects(request["source"].get<std::string>(), models, nameSpace);
            } else {
                response = modular ? pipelang::models::compileModules(modules, request["entry"].get<std::string>(), nameSpace)
                                   : pipelang::models::compile(request["source"].get<std::string>(), nameSpace);
            }
        } catch (const pipelang::models::Diagnostic& diagnostic) {
            json detail = {{"code", "PLM1001"}, {"message", diagnostic.what()}, {"offset", diagnostic.offset}};
            if (analysis || index) {
                detail["length"] = 0;
                for (const auto& module : modules) {
                    if ((!diagnostic.path.empty() && module.path != diagnostic.path) ||
                        (!diagnostic.module.empty() && module.name != diagnostic.module))
                        continue;
                    const auto lexical = pipelang::native::lex(module.source,
                        pipelang::native::NumericSyntax::Binary64, budget.tokens);
                    for (const auto& token : lexical.tokens) {
                        if (token.start == diagnostic.offset) {
                            detail["length"] = token.end - token.start;
                            break;
                        }
                    }
                    break;
                }
            }
            if (modular)
                detail["module"] = diagnostic.module;
            if ((project || index) && !diagnostic.path.empty())
                detail["path"] = diagnostic.path;
            response = {{"protocol", protocol}, {"ok", false}, {"diagnostics", json::array({detail})}};
        }
        const auto output = response.dump();
        if (pipelang::models::budgetExceeded(output.size(), 1, budget.outputBytes))
            throw std::runtime_error("response output_bytes resource budget exceeded; configure output_bytes");
        std::cout << output << '\n';
        if (!std::cout)
            throw std::runtime_error("failed to write response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-modelc: " << error.what() << '\n';
        return 2;
    }
}
