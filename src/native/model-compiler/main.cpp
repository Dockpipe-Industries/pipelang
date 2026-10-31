// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"

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

int main() {
    try {
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > pipelang::models::messageLimit - input.size())
                throw std::runtime_error("request byte limit exceeded");
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
        const bool objects = protocol == "pipelang.native-model.v3";
        const bool modular = protocol == "pipelang.native-model.v2" || (objects && request.contains("modules"));
        if (protocol != "pipelang.native-model.v1" && protocol != "pipelang.native-model.v2" && !objects)
            throw std::runtime_error("unsupported native model protocol");
        auto required = modular ? std::set<std::string>{"protocol", "modules", "entry"}
                                : std::set<std::string>{"protocol", "source"};
        if (objects)
            required.insert("models");
        requireFields(request, required, {"namespace"});
        std::vector<std::string> models;
        if (objects) {
            if (!request["models"].is_array() || request["models"].empty() || request["models"].size() > 128)
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
        if (modular) {
            if (!request["entry"].is_string() || !request["modules"].is_array() ||
                request["modules"].empty() || request["modules"].size() > 128)
                throw std::runtime_error("v2 requires an entry and 1..128 closed source modules");
            for (const auto& module : request["modules"]) {
                requireFields(module, {"name", "source"});
                if (!module["name"].is_string() || !module["source"].is_string())
                    throw std::runtime_error("module name and source must be strings");
                modules.push_back({module["name"].get<std::string>(), module["source"].get<std::string>()});
            }
        } else if (!request["source"].is_string()) {
            throw std::runtime_error("source must be a string");
        }
        json response;
        try {
            const auto nameSpace = request.value("namespace", "pipelang_models");
            if (objects) {
                response = modular
                    ? pipelang::models::compileModuleObjects(modules, request["entry"].get<std::string>(), models, nameSpace)
                    : pipelang::models::compileObjects(request["source"].get<std::string>(), models, nameSpace);
            } else {
                response = modular ? pipelang::models::compileModules(modules, request["entry"].get<std::string>(), nameSpace)
                                   : pipelang::models::compile(request["source"].get<std::string>(), nameSpace);
            }
        } catch (const pipelang::models::Diagnostic& diagnostic) {
            json detail = {{"code", "PLM1001"}, {"message", diagnostic.what()}, {"offset", diagnostic.offset}};
            if (modular)
                detail["module"] = diagnostic.module;
            response = {{"protocol", protocol}, {"ok", false}, {"diagnostics", json::array({detail})}};
        }
        const auto output = response.dump();
        if (output.size() + 1 > pipelang::models::messageLimit)
            throw std::runtime_error("response byte limit exceeded");
        std::cout << output << '\n';
        if (!std::cout)
            throw std::runtime_error("failed to write response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-modelc: " << error.what() << '\n';
        return 2;
    }
}
