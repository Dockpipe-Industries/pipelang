// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ProjectModelFacets.hpp"
#include "composition.hpp"

#include <map>
#include <utility>

namespace pipelang::models {
namespace {

class ProjectModelFacets {
public:
    ProjectModelFacets(Program& program,
                       const std::function<void(const Declaration&)>& claimClone)
        : program_(program), claimClone_(claimClone) {}

    void lower() {
        if (!program_.projectEnabled)
            return;

        // Record every identity before rewriting references between roots.
        for (const auto& declaration : program_.declarations) {
            if (declaration.modelExported)
                objectNames_.emplace(declaration.name, "pl_project_object_" + declaration.name);
        }

        std::vector<Declaration> objects;
        for (auto& declaration : program_.declarations) {
            if (!declaration.modelExported)
                continue;
            validateNativeShape(declaration);
            claimClone_(declaration);
            auto object = cloneDeclaration(declaration);
            object.name = objectNames_.at(declaration.name);
            object.modelExported = false;
            object.modelDeclared = false;
            object.generatedObjectFacet = true;
            for (auto& method : object.methods) {
                if (method.constructor)
                    method.name = object.name;
            }
            objects.push_back(std::move(object));

            declaration.sourceObjectFacet = objectNames_.at(declaration.name);
            // The source facet owns interface dispatch. A separately constructed
            // native owner has its own fixed receiver, never a borrowed ObjectRef.
            declaration.interfaces.clear();
            for (auto& field : declaration.fields)
                field.protectedMember = false;
            for (auto& method : declaration.methods) {
                method.protectedMember = false;
                method.virtualMethod = false;
            }
        }
        for (auto& object : objects)
            program_.declarations.push_back(std::move(object));
        for (auto& declaration : program_.declarations)
            rewriteDeclaration(declaration);
    }

private:
    [[noreturn]] static void refuse(const Declaration& declaration,
                                    const std::string& reason, std::size_t offset) {
        Diagnostic diagnostic("native owner facet of '" + declaration.sourceName + "' " + reason, offset);
        diagnostic.module = declaration.module;
        diagnostic.path = declaration.originPath;
        throw diagnostic;
    }

    static void validateReceiver(const Declaration& declaration, const Expr& expression) {
        if (!expression)
            return;
        if (expression->kind == "this" || expression->kind == "base") {
            refuse(declaration,
                   "does not yet support explicit this/base receiver references; ordinary unmarked classes retain these semantics",
                   expression->offset);
        }
        for (const auto& child : expression->children)
            validateReceiver(declaration, child);
    }

    static void validateStatements(const Declaration& declaration,
                                   const std::vector<Statement>& statements) {
        for (const auto& statement : statements) {
            validateReceiver(declaration, statement.expression);
            validateReceiver(declaration, statement.target);
            validateStatements(declaration, statement.initialization);
            validateStatements(declaration, statement.body);
            validateStatements(declaration, statement.iteration);
            validateStatements(declaration, statement.alternative);
            validateStatements(declaration, statement.cleanup);
        }
    }

    static void validateNativeShape(const Declaration& declaration) {
        if (!declaration.baseClass.empty()) {
            refuse(declaration, "does not yet support an inherited native owner; its source object facet is not an independent owner",
                   declaration.offset);
        }
        for (const auto& field : declaration.fields) {
            if (field.name == "Reference")
                refuse(declaration, "reserves the generated nested type name 'Reference'", field.offset);
            validateReceiver(declaration, field.initializer);
        }
        for (const auto& constant : declaration.constants) {
            if (constant.name == "Reference")
                refuse(declaration, "reserves the generated nested type name 'Reference'", constant.offset);
        }
        for (const auto& method : declaration.methods) {
            if (method.name == "Reference")
                refuse(declaration, "reserves the generated nested type name 'Reference'", method.offset);
            if (method.constructor && !method.visible)
                refuse(declaration, "requires public native constructors", method.offset);
            validateStatements(declaration, method.body);
            for (const auto& initializer : method.fieldInitializers)
                validateReceiver(declaration, initializer);
        }
    }

    void rewriteName(std::string& name) const {
        if (const auto found = objectNames_.find(name); found != objectNames_.end())
            name = found->second;
    }

    void rewriteType(Type& type) const {
        rewriteName(type.name);
        for (auto& argument : type.arguments)
            rewriteType(argument);
    }

    void rewriteExpression(Expr& expression) const {
        if (!expression)
            return;
        rewriteType(expression->type);
        rewriteType(expression->convertTo);
        for (auto& argument : expression->typeArguments)
            rewriteType(argument);
        if (expression->kind == "type")
            expression->text = expression->type.name;
        // A bare name may denote a local shadowing a type. Only owning Type
        // nodes are rewritten; constant-owner expressions still name the same
        // canonical declaration and are folded by the ordinary checker.
        for (auto& child : expression->children)
            rewriteExpression(child);
        rewriteExpression(expression->constantValue);
    }

    void rewriteField(Field& field, bool sourceObject) const {
        rewriteType(field.type);
        rewriteExpression(field.initializer);
        rewriteExpression(field.constantValue);
        if (sourceObject)
            rewriteName(field.declaringClass);
    }

    void rewriteStatements(std::vector<Statement>& statements) const {
        for (auto& statement : statements) {
            rewriteType(statement.type);
            rewriteType(statement.itemConvertTo);
            rewriteExpression(statement.expression);
            rewriteExpression(statement.target);
            for (auto& capture : statement.parallelCaptures)
                rewriteField(capture, true);
            rewriteStatements(statement.initialization);
            rewriteStatements(statement.body);
            rewriteStatements(statement.iteration);
            rewriteStatements(statement.alternative);
            rewriteStatements(statement.cleanup);
        }
    }

    void rewriteDeclaration(Declaration& declaration) const {
        const bool sourceObject = !declaration.modelExported;
        rewriteName(declaration.baseClass);
        for (auto& ancestor : declaration.ancestors)
            rewriteName(ancestor);
        for (auto& contract : declaration.interfaces)
            rewriteType(contract);
        for (auto& argument : declaration.genericArguments)
            rewriteType(argument);
        for (auto& field : declaration.fields)
            rewriteField(field, sourceObject);
        for (auto& constant : declaration.constants)
            rewriteField(constant, sourceObject);
        for (auto& method : declaration.methods) {
            rewriteType(method.result);
            for (auto& parameter : method.parameters)
                rewriteField(parameter, sourceObject);
            for (auto& initializer : method.fieldInitializers)
                rewriteExpression(initializer);
            rewriteExpression(method.baseConstructor);
            rewriteStatements(method.body);
        }
    }

    Program& program_;
    const std::function<void(const Declaration&)>& claimClone_;
    std::map<std::string, std::string> objectNames_;
};

}

void lowerProjectModelFacets(Program& program,
                            const std::function<void(const Declaration&)>& claimClone) {
    ProjectModelFacets(program, claimClone).lower();
}

}
