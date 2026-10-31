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
    std::string sourceName = {};
    std::string text() const;
    bool operator==(const Type& other) const {
        return name == other.name && arguments == other.arguments;
    }
};

inline bool isConcurrentCollection(const std::string& name) {
    return name == "ConcurrentDictionary" || name == "ConcurrentQueue" ||
        name == "ConcurrentStack";
}

inline bool isSynchronizationHandle(const std::string& name) {
    return name == "Lock" || name == "SemaphoreSlim";
}

inline bool isSharedHandle(const Type& type) {
    return type.name == "Atomic" || isConcurrentCollection(type.name) ||
        isSynchronizationHandle(type.name);
}

struct Expression {
    std::string kind;
    std::string text;
    std::size_t offset = 0;
    std::vector<std::shared_ptr<Expression>> children;
    Type type;
    // Semantic resolution is retained for code generation. Source spelling is
    // never used as unchecked native code.
    std::string resolution;
    std::vector<Type> typeArguments = {};
    std::string callable = {};
    std::string owner = {};
    std::vector<std::string> targets = {};
    Type convertTo = {};
};
using Expr = std::shared_ptr<Expression>;

struct Field {
    Type type;
    std::string name;
    bool visible = true;
    std::size_t offset = 0;
    Expr initializer = {};
    bool protectedMember = false;
    std::string declaringClass = {};
};

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
    std::vector<Field> parallelCaptures = {};
    Type itemConvertTo = {};
    std::size_t offset = 0;
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
    std::vector<std::string> typeParameters = {};
    std::string sourceName = {};
    std::string identity = {};
    std::set<std::string> parallelCalls = {};
    std::set<std::string> cleanupCalls = {};
    bool workerCallable = false;
    std::vector<Expr> fieldInitializers = {};
    std::set<std::string> lockCalls = {};
    bool protectedMember = false;
    bool virtualMethod = false;
    bool overrideMethod = false;
    Expr baseConstructor = {};
};

struct Declaration {
    std::string kind;
    std::string name;
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<std::string> values;
    std::size_t offset = 0;
    std::vector<std::string> typeParameters = {};
    std::string sourceName = {};
    std::string module = {};
    bool sourceInterface = false;
    bool modelExported = false;
    std::vector<Type> interfaces = {};
    std::string baseClass = {};
    std::vector<std::string> ancestors = {};
};

struct Program {
    std::vector<Declaration> declarations;
    std::string module = {};
    std::vector<std::string> imports = {};
    std::size_t tokenCount = 0;
    std::size_t expressionCount = 0;
    bool objectEnabled = false;
    std::vector<std::string> modelExports = {};
};

Program parse(const std::string& source, bool objectSyntax = false, std::size_t parameterLimit = 32);
void check(Program& program);
std::string generate(const Program& program, const std::string& nameSpace);
nlohmann::json manifest(const Program& program, const std::string& nameSpace);
std::string cppType(const Type& type, const Program& program);

}
