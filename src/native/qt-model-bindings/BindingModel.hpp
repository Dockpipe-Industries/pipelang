// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
#include "../third_party/nlohmann/json.hpp"

namespace pipelang::qt_bindings {
struct Property {
    std::string name;
    std::string type;
    bool getter = false;
};
struct Parameter { std::string name; std::string type; };
struct Action {
    std::string name;
    std::string result;
    std::vector<Parameter> parameters;
    bool pure = false;
};
struct BindingModel {
    std::string modelNamespace;
    std::string nativeModel;
    std::string sourceModel;
    std::vector<Property> properties;
    std::vector<Action> actions;
};
BindingModel admit(const nlohmann::json& manifest, const std::string& sourceModel);
std::string emit(const BindingModel& model, const std::string& nameSpace, const std::string& adapter);
bool identifier(const std::string& value);
}
