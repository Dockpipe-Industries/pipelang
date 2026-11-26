// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "syntax.hpp"

#include <algorithm>
#include <functional>
#include <string_view>

namespace pipelang::models {

inline bool programUsesType(const Program& program, std::string_view name)
{
    std::function<bool(const Type&)> inType = [&](const Type& type) {
        if (type.name == name)
            return true;
        return std::any_of(type.arguments.begin(), type.arguments.end(), inType);
    };
    std::function<bool(const Expr&)> inExpression = [&](const Expr& value) {
        return value && (inType(value->type) ||
            std::any_of(value->children.begin(), value->children.end(), inExpression));
    };
    std::function<bool(const std::vector<Statement>&)> inStatements = [&](const std::vector<Statement>& body) {
        return std::any_of(body.begin(), body.end(), [&](const Statement& statement) {
            return inType(statement.type) || inExpression(statement.expression) ||
                inExpression(statement.target) || inStatements(statement.body) || inStatements(statement.alternative) ||
                inStatements(statement.cleanup) || inStatements(statement.initialization) || inStatements(statement.iteration);
        });
    };
    for (const auto& declaration : program.declarations) {
        for (const auto& field : declaration.fields) {
            if (inType(field.type) || inExpression(field.initializer))
                return true;
        }
        for (const auto& method : declaration.methods) {
            if (inType(method.result) || inStatements(method.body))
                return true;
            for (const auto& initializer : method.fieldInitializers) {
                if (inExpression(initializer))
                    return true;
            }
            for (const auto& parameter : method.parameters) {
                if (inType(parameter.type))
                    return true;
            }
        }
    }
    return false;
}

}
