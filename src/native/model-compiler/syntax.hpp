// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compiler.hpp"
#include "CompiledProject.hpp"
#include "CompositionSyntax.hpp"
#include "../lexer/lexer.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace pipelang::models {

inline bool isConcurrentCollection(const std::string& name) {
    return name == "ConcurrentDictionary" || name == "ConcurrentQueue" ||
        name == "ConcurrentStack";
}

inline bool isSynchronizationHandle(const std::string& name) {
    return name == "Lock" || name == "SemaphoreSlim";
}

inline bool isSharedHandle(const Type& type) {
    return type.name == "Atomic" || isConcurrentCollection(type.name) ||
        isSynchronizationHandle(type.name) || type.name == "ContextOwner";
}

inline std::optional<Type> sequenceElementType(const Type& sequence)
{
    if (sequence.name == "ByteBuffer")
        return Type{"uint32", {}};
    if (sequence.name == "Dictionary" || sequence.name == "ConcurrentDictionary")
        return Type{"KeyValuePair", sequence.arguments};
    if (sequence.name == "List" || sequence.name == "Array" || sequence.name == "FixedArray" ||
        sequence.name == "HashSet" || sequence.name == "Queue" || sequence.name == "Stack" ||
        sequence.name == "ConcurrentQueue" || sequence.name == "ConcurrentStack")
        return sequence.arguments.at(0);
    return std::nullopt;
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
    // Only an authored implicit same-instance call may use receiver-free worker
    // lowering. Explicit receivers retain their ordinary ownership contract.
    bool implicitObjectCall = false;
    bool statelessWorkerCall = false;
    Type convertTo = {};
    std::shared_ptr<Expression> constantValue = {};
    // Project lexical linking proved this receiver denotes a source type.
    bool projectTypeReference = false;
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
    Expr constantValue = {};
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
    std::vector<Statement> initialization = {};
    std::vector<Statement> iteration = {};
    // Source provenance only; checked declarations always carry a concrete Type.
    bool inferredType = false;
    // Authored byte ranges retained for compiler-backed editor queries.
    std::size_t nameOffset = 0;
    std::size_t endOffset = 0;
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
    bool statelessWorkerCallable = false;
    std::vector<Expr> fieldInitializers = {};
    std::set<std::string> lockCalls = {};
    bool protectedMember = false;
    bool virtualMethod = false;
    bool overrideMethod = false;
    Expr baseConstructor = {};
    // Checked closed dispatch, retained for consumers selecting a method without
    // an authored call expression (for example a composition factory binding).
    std::vector<std::string> dispatchTargets = {};
    bool staticMethod = false;
    std::size_t nameOffset = 0;
    std::size_t endOffset = 0;
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
    std::string originPath = {};
    bool sourceInterface = false;
    bool modelExported = false;
    bool modelDeclared = false;
    // Project-only lowering facets share one authored identity.
    std::string sourceObjectFacet = {};
    bool generatedObjectFacet = false;
    std::string genericSourceName = {};
    std::vector<Type> genericArguments = {};
    std::vector<Type> interfaces = {};
    std::string baseClass = {};
    std::vector<std::string> ancestors = {};
    std::vector<Field> constants = {};
};

struct Program {
    std::vector<Declaration> declarations;
    std::vector<CompositionDeclaration> compositions;
    std::string module = {};
    std::vector<std::string> imports = {};
    std::vector<std::size_t> importOffsets = {};
    std::size_t tokenCount = 0;
    std::size_t expressionCount = 0;
    bool objectEnabled = false;
    bool projectEnabled = false;
    bool activeHostNumericEnvironment = false;
    std::map<std::string, std::size_t> projectGenericRecords = {};
    std::vector<std::string> modelExports = {};
};

Program parse(const std::string& source, bool objectSyntax = false, std::size_t parameterLimit = 32, bool projectSyntax = false);
// Inspect only the module/import prefix using the owning lexer and parser.
Program parseModuleHeader(const std::string& source);
// Project convention uses one ordinary source identifier as its implicit module.
std::string moduleIdentityFromStem(const std::string& stem);
// A normalizer-owned resolver admits concrete closed generic member applications.
// The checker remains the sole owner of receiver typing and call semantics.
using GenericMemberResolver = std::function<std::string(
    const std::string&, const std::string&, const std::vector<Type>&,
    const Declaration&, std::size_t)>;
void check(Program& program, const GenericMemberResolver& genericMembers = {});
std::string generate(const Program& program, const std::string& nameSpace);
std::string generateCompositionFactory(const Program& program, const std::string& nameSpace, bool compiled = false);
CompiledProject generateCompiledProject(const Program& program, const std::string& nameSpace);

std::string generateProjectTypes(const Program& program, const std::string& nameSpace);
std::string generateNumericHeader(const Program& program, const std::string& nameSpace);
Expr foldConstant(const Expr& expression);
std::string constantLiteral(const Expr& expression);
std::string constantsHeader(const Program& program, const std::string& nameSpace);
nlohmann::json constantsManifest(const Program& program, const std::string& nameSpace);
nlohmann::json manifest(const Program& program, const std::string& nameSpace);
std::string cppType(const Type& type, const Program& program);

}
