// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compiler.hpp"
#include "../lexer/lexer.hpp"
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace pipelang::models {

struct Type {
    std::string name;
    std::vector<Type> arguments;
    std::string text() const;
    bool operator==(const Type&) const = default;
};

struct Expression {
    std::string kind;
    std::string text;
    std::size_t offset = 0;
    std::vector<std::shared_ptr<Expression>> children;
    Type type;
    // Semantic resolution is retained for code generation. Source spelling is
    // never used as unchecked native code.
    std::string resolution;
};
using Expr = std::shared_ptr<Expression>;

struct Statement {
    std::string kind;
    std::string name;
    Type type;
    Expr expression;
    Expr target;
    std::vector<Statement> body;
    std::vector<Statement> alternative;
    std::vector<Statement> cleanup;
    bool hasFinally = false;
    bool bodyFallsThrough = true;
    std::size_t offset = 0;
};

struct Field {
    Type type;
    std::string name;
    bool visible = true;
    std::size_t offset = 0;
    Expr initializer;
};

struct Method {
    Type result;
    std::string name;
    std::vector<Field> parameters;
    std::vector<Statement> body;
    bool visible = true;
    bool pure = false;
    bool asynchronous = false;
    bool constructor = false;
    std::size_t offset = 0;
    std::set<std::string> calls;
};

struct Declaration {
    std::string kind;
    std::string name;
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<std::string> values;
    std::size_t offset = 0;
};

struct Program {
    std::vector<Declaration> declarations;
};

Program parse(const std::string& source);
void check(Program& program);
std::string generate(const Program& program, const std::string& nameSpace);
nlohmann::json manifest(const Program& program, const std::string& nameSpace);
std::string cppType(const Type& type, const Program& program);

}
