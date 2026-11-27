// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "composition.hpp"
#include "CheckedProgram.hpp"
#include "ProjectModelFacets.hpp"
#include "DiagnosticOriginScope.hpp"
#include "ModuleDiagnosticScope.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <tuple>
#include <iterator>

namespace pipelang::models {
namespace {

using Substitutions = std::map<std::string, Type>;

[[noreturn]] void fail(const std::string& message, std::size_t offset,
                       const std::string& module = {}) {
    Diagnostic diagnostic(message, offset);
    diagnostic.module = module;
    diagnostic.path = DiagnosticOriginScope::current();
    throw diagnostic;
}

bool builtin(const std::string& name) {
    static const std::set<std::string> names = {
        "void", "bool", "int", "uint32", "float", "float32", "string", "ByteBuffer", "Error", "CancellationToken",
        "Result", "IResult", "Task", "Optional", "List", "Atomic", "Array", "FixedArray", "Dictionary",
        "HashSet", "Queue", "Stack", "KeyValuePair",
        "ConcurrentDictionary", "ConcurrentQueue", "ConcurrentStack", "Lock", "SemaphoreSlim", "ContextOwner"
    };
    return names.contains(name);
}

bool moduleName(const std::string& value) {
    if (value.empty() || value.size() > 192)
        return false;
    bool initial = true;
    for (const unsigned char character : value) {
        if (character == '.') {
            if (initial)
                return false;
            initial = true;
        } else {
            const bool letter = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            if (!letter && character != '_' && (initial || !digit))
                return false;
            initial = false;
        }
    }
    return !initial;
}

// Claim the entire tree before a clone or type substitution allocates storage.
// The counter is cumulative, including temporary generic binding/type copies.
class SpecializationBudget {
public:
    void type(const Type& value, std::size_t offset, const std::string& module, unsigned depth = 0) {
        if (value.name.empty())
            return;
        if (depth > 12)
            fail("closed type nesting limit exceeded", offset, module);
        claim(offset, module);
        for (const auto& argument : value.arguments)
            type(argument, offset, module, depth + 1);
    }

    void expression(const Expr& value, std::size_t offset, const std::string& module) {
        if (!value)
            return;
        claim(offset, module);
        type(value->type, offset, module);
        for (const auto& argument : value->typeArguments)
            type(argument, offset, module);
        for (const auto& child : value->children)
            expression(child, offset, module);
    }

    void field(const Field& value, std::size_t offset, const std::string& module) {
        claim(offset, module);
        type(value.type, offset, module);
        expression(value.initializer, offset, module);
    }

    void statement(const Statement& value, std::size_t offset, const std::string& module) {
        claim(offset, module);
        type(value.type, offset, module);
        expression(value.expression, offset, module);
        expression(value.target, offset, module);
        for (const auto& capture : value.parallelCaptures)
            field(capture, offset, module);
        for (const auto& child : value.initialization)
            statement(child, offset, module);
        for (const auto& child : value.body)
            statement(child, offset, module);
        for (const auto& child : value.iteration)
            statement(child, offset, module);
        for (const auto& child : value.alternative)
            statement(child, offset, module);
        for (const auto& child : value.cleanup)
            statement(child, offset, module);
    }

    void method(const Method& value, std::size_t offset, const std::string& module) {
        claim(offset, module);
        type(value.result, offset, module);
        for (const auto& parameter : value.parameters)
            field(parameter, offset, module);
        expression(value.baseConstructor, offset, module);
        for (const auto& initializer : value.fieldInitializers)
            expression(initializer, offset, module);
        for (const auto& child : value.body)
            statement(child, offset, module);
    }

    void record(const Declaration& value, std::size_t offset, const std::string& module) {
        claim(offset, module);
        for (const auto& member : value.fields)
            field(member, offset, module);
        for (const auto& member : value.constants)
            field(member, offset, module);
    }

    // Project facets copy a normalized declaration, including semantic metadata.
    // Keep this complete traversal separate from legacy specialization admission.
    void projectDeclaration(const Declaration& value) {
        claim(value.offset, value.module);
        for (const auto& argument : value.genericArguments)
            projectType(argument, value.offset, value.module);
        for (const auto& contract : value.interfaces)
            projectType(contract, value.offset, value.module);
        for (const auto& member : value.fields)
            projectField(member, value.module);
        for (const auto& constant : value.constants)
            projectField(constant, value.module);
        for (const auto& method : value.methods) {
            claim(method.offset, value.module);
            projectType(method.result, method.offset, value.module);
            for (const auto& parameter : method.parameters)
                projectField(parameter, value.module);
            projectExpression(method.baseConstructor, value.module);
            for (const auto& initializer : method.fieldInitializers)
                projectExpression(initializer, value.module);
            for (const auto& statement : method.body)
                projectStatement(statement, value.module);
        }
    }

private:
    void projectType(const Type& value, std::size_t offset, const std::string& module, unsigned depth = 0) {
        if (value.name.empty() && value.sourceName.empty() && value.arguments.empty())
            return;
        if (depth > 12)
            fail("closed type nesting limit exceeded", offset, module);
        claim(offset, module);
        for (const auto& argument : value.arguments)
            projectType(argument, offset, module, depth + 1);
    }

    void projectExpression(const Expr& value, const std::string& module) {
        if (!value)
            return;
        claim(value->offset, module);
        projectType(value->type, value->offset, module);
        projectType(value->convertTo, value->offset, module);
        for (const auto& argument : value->typeArguments)
            projectType(argument, value->offset, module);
        for (const auto& child : value->children)
            projectExpression(child, module);
        projectExpression(value->constantValue, module);
    }

    void projectField(const Field& value, const std::string& module) {
        claim(value.offset, module);
        projectType(value.type, value.offset, module);
        projectExpression(value.initializer, module);
        projectExpression(value.constantValue, module);
    }

    void projectStatement(const Statement& value, const std::string& module) {
        claim(value.offset, module);
        projectType(value.type, value.offset, module);
        projectType(value.itemConvertTo, value.offset, module);
        projectExpression(value.expression, module);
        projectExpression(value.target, module);
        for (const auto& capture : value.parallelCaptures)
            projectField(capture, module);
        for (const auto& child : value.initialization)
            projectStatement(child, module);
        for (const auto& child : value.body)
            projectStatement(child, module);
        for (const auto& child : value.iteration)
            projectStatement(child, module);
        for (const auto& child : value.alternative)
            projectStatement(child, module);
        for (const auto& child : value.cleanup)
            projectStatement(child, module);
    }

    void claim(std::size_t offset, const std::string& module) {
        if (used_ == maximumNodes)
            fail("closed specialization AST/type node budget exceeded", offset, module);
        ++used_;
    }

    static constexpr std::size_t maximumNodes = 65536;
    std::size_t used_ = 0;
};

Expr cloneExpression(const Expr& original) {
    if (!original)
        return {};
    auto result = std::make_shared<Expression>(*original);
    result->constantValue = cloneExpression(original->constantValue);
    result->children.clear();
    for (const auto& child : original->children)
        result->children.push_back(cloneExpression(child));
    return result;
}

Statement cloneStatement(const Statement& original) {
    Statement result = original;
    result.expression = cloneExpression(original.expression);
    result.target = cloneExpression(original.target);
    result.initialization.clear();
    result.iteration.clear();
    result.body.clear();
    result.alternative.clear();
    result.cleanup.clear();
    for (auto& capture : result.parallelCaptures) {
        capture.initializer = cloneExpression(capture.initializer);
        capture.constantValue = cloneExpression(capture.constantValue);
    }
    for (const auto& statement : original.initialization)
        result.initialization.push_back(cloneStatement(statement));
    for (const auto& statement : original.iteration)
        result.iteration.push_back(cloneStatement(statement));
    for (const auto& statement : original.body)
        result.body.push_back(cloneStatement(statement));
    for (const auto& statement : original.alternative)
        result.alternative.push_back(cloneStatement(statement));
    for (const auto& statement : original.cleanup)
        result.cleanup.push_back(cloneStatement(statement));
    return result;
}

Method cloneMethod(const Method& original) {
    Method result = original;
    result.body.clear();
    result.baseConstructor = cloneExpression(original.baseConstructor);
    result.fieldInitializers.clear();
    for (const auto& initializer : original.fieldInitializers)
        result.fieldInitializers.push_back(cloneExpression(initializer));
    for (auto& parameter : result.parameters) {
        parameter.initializer = cloneExpression(parameter.initializer);
        parameter.constantValue = cloneExpression(parameter.constantValue);
    }
    for (const auto& statement : original.body)
        result.body.push_back(cloneStatement(statement));
    return result;
}

std::string authoredName(const Declaration& declaration) {
    return declaration.sourceName.empty() ? declaration.name : declaration.sourceName;
}

std::string appliedName(const std::string& name, const std::vector<Type>& arguments) {
    std::string result = name + '<';
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0)
            result += ',';
        result += arguments[index].text();
    }
    result += '>';
    if (result.size() > 4096)
        fail("closed generic identity exceeds its limit", 0);
    return result;
}

std::vector<std::reference_wrapper<Field>> fieldsAndConstants(Declaration& declaration) {
    std::vector<std::reference_wrapper<Field>> members;
    members.reserve(declaration.fields.size() + declaration.constants.size());
    for (auto& field : declaration.fields)
        members.push_back(field);
    for (auto& constant : declaration.constants)
        members.push_back(constant);
    return members;
}

std::vector<std::reference_wrapper<const Field>> fieldsAndConstants(const Declaration& declaration) {
    std::vector<std::reference_wrapper<const Field>> members;
    members.reserve(declaration.fields.size() + declaration.constants.size());
    for (const auto& field : declaration.fields)
        members.push_back(field);
    for (const auto& constant : declaration.constants)
        members.push_back(constant);
    return members;
}

class ModuleLinker {
public:
    Program run(const std::vector<ModuleSource>& sources, const std::string& entry, bool objectSyntax, bool projectSyntax) {
        DiagnosticOriginScope compilationOrigin(std::string{});
        const auto& budget = resourceBudget();
        const std::size_t sourceCountLimit = projectSyntax ? budget.sourceFragments : budget.modules;
        if (sources.empty() || sources.size() > sourceCountLimit || !moduleName(entry))
            fail("source fragment resource budget exceeded or invalid entry; configure source_fragments/modules", 0);
        std::vector<const ModuleSource*> fragments;
        for (const auto& source : sources)
            fragments.push_back(&source);
        if (projectSyntax) {
            std::sort(fragments.begin(), fragments.end(), [](const ModuleSource* left, const ModuleSource* right) {
                return std::tie(left->name, left->path) < std::tie(right->name, right->path);
            });
        }
        std::size_t sourceBytes = 0;
        std::size_t originBytes = 0;
        std::size_t declarationCount = 0;
        std::size_t tokenCount = 0;
        std::size_t expressionCount = 0;
        std::set<std::string> sourcePaths;
        std::map<std::string, bool> explicitModules;
        for (const auto* fragment : fragments) {
            const auto& source = *fragment;
            if (projectSyntax) {
                if (budgetExceeded(originBytes, source.path.size(), budget.outputBytes))
                    fail("project fragment origin receipt byte limit exceeded", 0, source.name);
                originBytes += source.path.size();
            }
            DiagnosticOriginScope origin(source.path);
            if (budgetExceeded(sourceBytes, source.source.size(), budget.inputBytes))
                fail("aggregate input_bytes resource budget exceeded; configure input_bytes", 0, source.name);
            sourceBytes += source.source.size();
            if (!moduleName(source.name) || (!projectSyntax && programs_.contains(source.name)))
                fail("invalid or duplicate supplied module '" + source.name + "'", 0, source.name);
            if (projectSyntax)
                validateProjectModuleName(source.name);
            if (projectSyntax && !source.path.empty()) {
                if (!projectSourcePath(source.path) || !sourcePaths.insert(source.path).second)
                    fail("invalid or duplicate project fragment path '" + source.path + "'", 0, source.name);
            }
            Program program;
            try {
                program = parse(source.source, objectSyntax, 32, projectSyntax);
            } catch (Diagnostic& diagnostic) {
                diagnostic.module = source.name;
                diagnostic.path = source.path;
                throw;
            }
            const bool explicitModule = !program.module.empty();
            if (projectSyntax && program.module.empty())
                program.module = source.name;
            if (program.module != source.name)
                fail("source module declaration must match its supplied module identity", 0, source.name);
            if (budgetExceeded(tokenCount, program.tokenCount, budget.tokens))
                fail("aggregate module token resource budget exceeded; configure tokens", 0, source.name);
            tokenCount += program.tokenCount;
            if (budgetExceeded(expressionCount, program.expressionCount, budget.expressions))
                fail("aggregate module expression resource budget exceeded; configure expressions", 0, source.name);
            expressionCount += program.expressionCount;
            if (budgetExceeded(declarationCount, program.declarations.size(), budget.declarations))
                fail("aggregate module declaration resource budget exceeded; configure declarations", 0, source.name);
            declarationCount += program.declarations.size();
            if (budgetExceeded(declarationCount, program.compositions.size(), budget.declarations))
                fail("aggregate module declaration resource budget exceeded; configure declarations", 0, source.name);
            declarationCount += program.compositions.size();
            if (programs_.contains(source.name)) {
                if (!explicitModule || !explicitModules.at(source.name) || source.path.empty() ||
                    modulePaths_.at(source.name).empty())
                    fail("duplicate project module requires distinct explicit source parts: " +
                         modulePaths_.at(source.name) + " and " + source.path, 0, source.name);
            } else {
                if (programs_.size() == budget.modules)
                    fail("project module resource budget exceeded; configure modules", 0, source.name);
                explicitModules.emplace(source.name, explicitModule);
                modulePaths_.emplace(source.name, source.path);
            }
            std::set<std::string> imports;
            for (std::size_t index = 0; index < program.imports.size(); ++index) {
                if (!projectSyntax)
                    continue;
                const auto& dependency = program.imports[index];
                if (dependency == source.name || !imports.insert(dependency).second)
                    fail("self or duplicate module import", program.importOffsets.at(index), source.name);
                importOrigins_.emplace(std::make_pair(source.name, dependency),
                                       std::make_pair(source.path, program.importOffsets.at(index)));
            }
            lexicalImports_.emplace(std::make_pair(source.name, source.path), program.imports);
            for (auto& declaration : program.declarations) {
                declaration.originPath = source.path;
                if (projectSyntax)
                    declaration.module = source.name;
            }
            for (auto& composition : program.compositions) {
                composition.originPath = source.path;
                composition.module = source.name;
            }
            if (const auto existing = programs_.find(source.name); existing != programs_.end()) {
                auto& combined = existing->second;
                combined.declarations.insert(combined.declarations.end(),
                    std::make_move_iterator(program.declarations.begin()), std::make_move_iterator(program.declarations.end()));
                combined.compositions.insert(combined.compositions.end(),
                    std::make_move_iterator(program.compositions.begin()), std::make_move_iterator(program.compositions.end()));
                std::set<std::string> dependencies(combined.imports.begin(), combined.imports.end());
                dependencies.insert(program.imports.begin(), program.imports.end());
                combined.imports.assign(dependencies.begin(), dependencies.end());
            } else {
                programs_.emplace(source.name, std::move(program));
            }
        }
        if (!programs_.contains(entry))
            fail("entry module was not supplied", 0, entry);
        for (const auto& [name, program] : programs_) {
            std::set<std::string> imported;
            for (const auto& dependency : program.imports) {
                if (!programs_.contains(dependency)) {
                    if (projectSyntax) {
                        const auto& [path, offset] = importOrigins_.at({name, dependency});
                        DiagnosticOriginScope origin(path);
                        fail("imported module was not supplied: " + dependency, offset, name);
                    }
                    fail("imported module was not supplied: " + dependency, 0, name);
                }
                if (dependency == name || !imported.insert(dependency).second)
                    fail("self or duplicate module import", 0, name);
            }
        }
        std::set<std::string> visiting;
        std::set<std::string> visited;
        struct ImportFrame {
            std::string name;
            std::size_t next = 0;
        };
        // Validate every supplied module, including modules outside the entry closure.
        for (const auto& [name, program] : programs_) {
            (void)program;
            if (visited.contains(name))
                continue;
            std::vector<ImportFrame> pending{{name, 0}};
            visiting.insert(name);
            while (!pending.empty()) {
                auto& frame = pending.back();
                const auto& imports = programs_.at(frame.name).imports;
                if (frame.next == imports.size()) {
                    visiting.erase(frame.name);
                    visited.insert(frame.name);
                    pending.pop_back();
                    continue;
                }
                const auto dependency = imports[frame.next++];
                if (visiting.contains(dependency)) {
                    if (projectSyntax) {
                        const auto& [path, offset] = importOrigins_.at({frame.name, dependency});
                        DiagnosticOriginScope origin(path);
                        fail("module import cycle", offset, frame.name);
                    }
                    fail("module import cycle", 0, dependency);
                }
                if (!visited.contains(dependency)) {
                    visiting.insert(dependency);
                    pending.push_back({dependency, 0});
                }
            }
        }
        std::set<std::string> reachable;
        std::vector<std::string> pending{entry};
        while (!pending.empty()) {
            auto name = std::move(pending.back());
            pending.pop_back();
            if (!reachable.insert(name).second)
                continue;
            const auto& imports = programs_.at(name).imports;
            pending.insert(pending.end(), imports.begin(), imports.end());
        }
        if (reachable.size() != programs_.size())
            fail("supplied module is outside the entry import closure", 0, entry);

        std::size_t identity = 0;
        for (auto& [name, program] : programs_) {
            std::map<std::string, const Declaration*> localNames;
            for (auto& declaration : program.declarations) {
                DiagnosticOriginScope origin(declaration.originPath);
                if (const auto previous = localNames.find(declaration.name); previous != localNames.end()) {
                    if (!projectSyntax)
                        fail("duplicate or reserved module type", declaration.offset, name);
                    const auto& first = *previous->second;
                    fail("duplicate module declaration '" + name + '.' + declaration.name + "' in " +
                         first.originPath + " at offset " + std::to_string(first.offset) + " and " +
                         declaration.originPath + " at offset " + std::to_string(declaration.offset),
                         declaration.offset, name);
                }
                if (builtin(declaration.name))
                    fail("duplicate or reserved module type", declaration.offset, name);
                localNames.emplace(declaration.name, &declaration);
                const auto canonical = name + '.' + declaration.name;
                // Preserve the entry class names used by native host consumers.
                names_[canonical] = name == entry && !projectSyntax ? declaration.name
                    : "pl_module_type_" + std::to_string(identity++);
            }
            for (auto& composition : program.compositions) {
                DiagnosticOriginScope origin(composition.originPath);
                const auto canonical = name + '.' + composition.name;
                if (localNames.contains(composition.name) || builtin(composition.name) ||
                    compositionNames_.contains(canonical))
                    fail("duplicate or reserved composition declaration '" + canonical + "'",
                         composition.offset, name);
                compositionNames_.emplace(canonical, canonical);
            }
        }
        const auto inheritedNames = objectSyntax ? inheritedClassNames() : std::map<std::string, std::set<std::string>>{};
        Program linked;
        linked.module = entry;
        linked.projectEnabled = projectSyntax;
        linked.tokenCount = tokenCount;
        linked.expressionCount = expressionCount;
        for (auto& [name, program] : programs_) {
            current_ = &program;
            for (auto& declaration : program.declarations) {
                DiagnosticOriginScope origin(declaration.originPath);
                currentImports_ = &lexicalImports_.at({name, declaration.originPath});
                const auto canonical = name + '.' + declaration.name;
                declaration.sourceName = canonical;
                declaration.module = name;
                declaration.name = names_.at(canonical);
                std::set<std::string> parameters(declaration.typeParameters.begin(),
                                                 declaration.typeParameters.end());
                for (auto& interface : declaration.interfaces)
                    rewriteType(interface, parameters, declaration.offset);
                std::set<std::string> values;
                for (const Field& field : fieldsAndConstants(declaration))
                    values.insert(field.name);
                if (const auto inherited = inheritedNames.find(canonical); inherited != inheritedNames.end())
                    values.insert(inherited->second.begin(), inherited->second.end());
                for (Field& field : fieldsAndConstants(declaration)) {
                    rewriteType(field.type, parameters, field.offset);
                    rewriteExpression(field.initializer, parameters, values);
                }
                for (auto& method : declaration.methods) {
                    if (method.constructor)
                        method.name = declaration.name;
                    auto methodParameters = parameters;
                    methodParameters.insert(method.typeParameters.begin(), method.typeParameters.end());
                    rewriteType(method.result, methodParameters, method.offset);
                    auto methodValues = values;
                    for (auto& parameter : method.parameters) {
                        rewriteType(parameter.type, methodParameters, parameter.offset);
                        methodValues.insert(parameter.name);
                    }
                    rewriteExpression(method.baseConstructor, methodParameters, methodValues);
                    rewriteStatements(method.body, methodParameters, methodValues);
                }
                linked.declarations.push_back(std::move(declaration));
            }
            for (auto& composition : program.compositions) {
                DiagnosticOriginScope origin(composition.originPath);
                currentImports_ = &lexicalImports_.at({name, composition.originPath});
                composition.sourceName = name + '.' + composition.name;
                composition.name = composition.sourceName;
                for (auto& registration : composition.registrations) {
                    rewriteType(registration.service, {}, registration.offset);
                    rewriteType(registration.implementation, {}, registration.offset);
                    if (registration.factory)
                        rewriteType(registration.factory->owner, {}, registration.factory->offset);
                }
                for (auto& cleanup : composition.cleanupContracts)
                    rewriteType(cleanup.owner, {}, cleanup.offset);
                for (std::size_t index = 0; index < composition.exports.size(); ++index)
                    rewriteType(composition.exports[index], {}, composition.exportOffsets.at(index));
                for (auto& include : composition.includes)
                    include.name = resolve(include.name, include.offset, true, true);
                linked.compositions.push_back(std::move(composition));
            }
        }
        return linked;
    }

private:
    std::map<std::string, std::set<std::string>> inheritedClassNames() {
        std::map<std::string, const Declaration*> classes;
        std::map<std::string, std::set<std::string>> members;
        std::map<std::string, std::vector<std::string>> bases;
        for (auto& [module, program] : programs_) {
            for (const auto& declaration : program.declarations) {
                DiagnosticOriginScope origin(declaration.originPath);
                currentImports_ = &lexicalImports_.at({module, declaration.originPath});
                if (declaration.kind != "Class")
                    continue;
                const auto canonical = module + '.' + declaration.name;
                classes.emplace(canonical, &declaration);
                for (const Field& field : fieldsAndConstants(declaration))
                    members[canonical].insert(field.name);
                for (const auto& method : declaration.methods) {
                    if (!method.constructor)
                        members[canonical].insert(method.name);
                }
            }
        }
        for (auto& [module, program] : programs_) {
            current_ = &program;
            for (const auto& declaration : program.declarations) {
                DiagnosticOriginScope origin(declaration.originPath);
                currentImports_ = &lexicalImports_.at({module, declaration.originPath});
                if (declaration.kind != "Class")
                    continue;
                const auto canonical = module + '.' + declaration.name;
                for (const auto& base : declaration.interfaces) {
                    const auto resolved = resolve(base.name, declaration.offset, false);
                    if (classes.contains(resolved))
                        bases[canonical].push_back(resolved);
                }
            }
        }
        // Resolve lexical member names before nominal rewriting consumes the
        // original declarations. Iterative closure is bounded even for cycles;
        // hierarchy validation later owns the cycle/depth diagnostics.
        for (std::size_t pass = 0; pass < classes.size(); ++pass) {
            bool changed = false;
            for (const auto& [owner, ancestors] : bases) {
                for (const auto& ancestor : ancestors) {
                    for (const auto& name : members[ancestor]) {
                        if (members[owner].contains(name))
                            continue;
                        if (members[owner].size() == 256) {
                            DiagnosticOriginScope origin(classes.at(owner)->originPath);
                            fail("class inherited member name limit exceeded", classes.at(owner)->offset, classes.at(owner)->module);
                        }
                        members[owner].insert(name);
                        changed = true;
                    }
                }
            }
            if (!changed)
                break;
        }
        std::map<std::string, std::set<std::string>> result;
        for (const auto& [owner, ancestors] : bases)
            result.emplace(owner, std::move(members[owner]));
        return result;
    }

    std::string resolve(const std::string& name, std::size_t offset, bool required,
                        bool composition = false) const {
        const auto& identities = composition ? compositionNames_ : names_;
        const std::string kind = composition ? "composition" : "type";
        const auto local = current_->module + '.' + name;
        if (identities.contains(local))
            return local;
        std::vector<std::string> candidates;
        for (const auto& dependency : *currentImports_) {
            const auto candidate = dependency + '.' + name;
            if (identities.contains(candidate))
                candidates.push_back(candidate);
        }
        if (candidates.size() > 1)
            fail("ambiguous imported " + kind + " '" + name + "'", offset, current_->module);
        if (candidates.size() == 1)
            return candidates.front();
        if (identities.contains(name)) {
            const auto separator = name.rfind('.');
            const auto owner = name.substr(0, separator);
            if (owner == current_->module ||
                std::find(currentImports_->begin(), currentImports_->end(), owner) != currentImports_->end())
                return name;
            fail("qualified " + kind + " requires an explicit direct import", offset, current_->module);
        }
        if (required)
            fail("unknown or unimported " + kind + " '" + name + "'", offset, current_->module);
        return {};
    }

    void rewriteType(Type& type, const std::set<std::string>& parameters, std::size_t offset) {
        if (type.name.empty())
            return;
        for (auto& argument : type.arguments)
            rewriteType(argument, parameters, offset);
        if (builtin(type.name) || parameters.contains(type.name))
            return;
        if (type.name == "var" && resolve(type.name, offset, false).empty())
            fail("var is only valid for inferred locals; API types must be explicit", offset);
        const auto canonical = resolve(type.name, offset, true);
        type.name = names_.at(canonical);
        type.sourceName = canonical;
    }

    static std::string expressionPath(const Expr& expression) {
        if (expression->kind == "name" || expression->kind == "generic_name")
            return expression->text;
        if ((expression->kind == "member" || expression->kind == "generic_member") &&
            expression->children.size() == 1) {
            const auto prefix = expressionPath(expression->children.front());
            if (!prefix.empty())
                return prefix + '.' + expression->text;
        }
        return {};
    }

    void rewriteExpression(Expr& expression, const std::set<std::string>& parameters,
                           const std::set<std::string>& values) {
        if (!expression)
            return;
        for (auto& argument : expression->typeArguments)
            rewriteType(argument, parameters, expression->offset);
        if (expression->kind == "type") {
            rewriteType(expression->type, parameters, expression->offset);
            expression->text = expression->type.text();
        }
        if (expression->kind == "named_construct" || expression->kind == "new_construct") {
            rewriteType(expression->type, parameters, expression->offset);
            expression->text = expression->type.name;
            if (expression->kind == "new_construct")
                expression->children[0]->text = expression->type.name;
        } else {
            const auto path = expressionPath(expression);
            const auto separator = path.find('.');
            const auto first = path.substr(0, separator);
            if (!path.empty() && !values.contains(first) && !parameters.contains(first)) {
                const auto canonical = resolve(path, expression->offset, false);
                if (!canonical.empty()) {
                    expression->kind = expression->typeArguments.empty() ? "name" : "generic_name";
                    expression->text = names_.at(canonical);
                    expression->projectTypeReference = expression->kind == "name";
                    expression->children.clear();
                }
            }
        }
        for (auto& child : expression->children)
            rewriteExpression(child, parameters, values);
    }

    void rewriteStatements(std::vector<Statement>& statements, const std::set<std::string>& parameters,
                           std::set<std::string> values) {
        for (auto& statement : statements) {
            if (statement.type.name.empty() && (statement.kind == "local" ||
                statement.kind == "foreach" || statement.kind == "parallel_foreach")) {
                const auto contextual = resolve("var", statement.offset, false);
                if (parameters.contains("var") || !contextual.empty())
                    statement.type = {"var", {}};
            }
            rewriteType(statement.type, parameters, statement.offset);
            if (statement.kind == "for") {
                rewriteStatements(statement.initialization, parameters, values);
                auto loopValues = values;
                for (const auto& initializer : statement.initialization) {
                    if (initializer.kind == "local")
                        loopValues.insert(initializer.name);
                }
                rewriteExpression(statement.expression, parameters, loopValues);
                rewriteStatements(statement.body, parameters, loopValues);
                rewriteStatements(statement.iteration, parameters, loopValues);
                continue;
            }
            rewriteExpression(statement.expression, parameters, values);
            rewriteExpression(statement.target, parameters, values);
            auto bodyValues = values;
            auto alternativeValues = values;
            if (statement.kind == "foreach" || statement.kind == "parallel_foreach")
                bodyValues.insert(statement.name);
            if (statement.kind == "try" && !statement.name.empty())
                alternativeValues.insert(statement.name);
            rewriteStatements(statement.body, parameters, bodyValues);
            rewriteStatements(statement.alternative, parameters, alternativeValues);
            rewriteStatements(statement.cleanup, parameters, values);
            if (statement.kind == "local")
                values.insert(statement.name);
        }
    }

    static void validateProjectModuleName(const std::string& module) {
        std::size_t begin = 0;
        while (begin < module.size()) {
            const auto end = module.find('.', begin);
            const auto segment = module.substr(begin, end == std::string::npos ? end : end - begin);
            try {
                moduleIdentityFromStem(segment);
            } catch (const Diagnostic& diagnostic) {
                fail("invalid project module identifier '" + segment + "': " + diagnostic.what(), 0, module);
            }
            if (end == std::string::npos)
                return;
            begin = end + 1;
        }
    }
    static bool projectSourcePath(const std::string& path) {
        if (path.empty() || path.size() > sourcePathLimit || path.front() == '/' || !path.ends_with(".pipe") ||
            path.find_first_of("\\:\r\n;") != std::string::npos || path.find('\0') != std::string::npos)
            return false;
        std::size_t begin = 0;
        while (begin < path.size()) {
            const auto end = path.find('/', begin);
            const auto component = path.substr(begin, end == std::string::npos ? end : end - begin);
            if (component.empty() || component == "." || component == "..")
                return false;
            if (end == std::string::npos)
                return true;
            begin = end + 1;
        }
        return false;
    }
    std::map<std::string, Program> programs_;
    std::map<std::string, std::string> modulePaths_;
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> lexicalImports_;
    std::map<std::pair<std::string, std::string>, std::pair<std::string, std::size_t>> importOrigins_;
    const std::vector<std::string>* currentImports_ = nullptr;
    std::map<std::string, std::string> names_;
    std::map<std::string, std::string> compositionNames_;
    Program* current_ = nullptr;
};

class CompositionNormalizer {
public:
    explicit CompositionNormalizer(Program& program) : program_(program) {
        for (const auto& declaration : program_.declarations)
            declarationOrigins_.emplace(declaration.name, std::make_pair(declaration.module, declaration.originPath));
    }

    void run(bool checkSemantics = false) {
        // Discover the additive syntax before specializing any local helper.
        // Programs without instance generic syntax retain their original native
        // specialization names and avoid checker discovery/cloning entirely.
        for (auto& declaration : program_.declarations) {
            for (const Field& field : fieldsAndConstants(declaration))
                hasGenericMembers_ = hasGenericMembers_ || genericMember(field.initializer);
            for (const auto& method : declaration.methods) {
                hasGenericMembers_ = hasGenericMembers_ || genericMember(method.baseConstructor) ||
                    genericMember(method.body);
                for (const auto& initializer : method.fieldInitializers)
                    hasGenericMembers_ = hasGenericMembers_ || genericMember(initializer);
            }
        }
        resolveModelExports();
        // Store templates separately so vector growth never invalidates their identity.
        std::vector<Declaration> concrete;
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            validateParameters(declaration.typeParameters, declaration.offset, declaration.module);
            if (!declaration.typeParameters.empty()) {
                if (program_.projectEnabled)
                    program_.projectGenericRecords.emplace(declaration.sourceName, declaration.typeParameters.size());
                if (declaration.kind != "Record")
                    fail("user generic declarations currently require Record", declaration.offset, declaration.module);
                if (!declaration.methods.empty())
                    fail("generic records declare owned value fields only", declaration.offset, declaration.module);
                std::set<std::string> fieldNames;
                for (const Field& field : fieldsAndConstants(declaration)) {
                    if (!field.visible || !fieldNames.insert(field.name).second)
                        fail("generic record fields must be public and unique", field.offset, declaration.module);
                }
                const auto name = declaration.name;
                const auto offset = declaration.offset;
                const auto module = declaration.module;
                if (!records_.emplace(name, std::move(declaration)).second)
                    fail("duplicate generic record", offset, module);
            } else {
                if (program_.projectEnabled && declaration.kind == "Class" && declaration.interfaces.empty())
                    directProjectClasses_.insert(declaration.name);

                concrete.push_back(std::move(declaration));
            }
        }
        program_.declarations = std::move(concrete);
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (records_.contains(declaration.name))
                fail("generic and ordinary type names collide", declaration.offset, declaration.module);
            std::vector<Method> methods;
            for (auto& method : declaration.methods) {
                validateParameters(method.typeParameters, method.offset, declaration.module);
                if (!method.typeParameters.empty() && (method.virtualMethod || method.overrideMethod))
                    fail("generic virtual methods are not supported", method.offset, declaration.module);
                if (method.typeParameters.empty()) {
                    methods.push_back(std::move(method));
                } else {
                    if (declaration.kind != "Class" || method.constructor)
                        fail("generic methods currently require an ordinary Class method", method.offset, declaration.module);
                    const auto name = method.name;
                    templates_[declaration.name][name].push_back(std::move(method));
                }
            }
            declaration.methods = std::move(methods);
        }
        // Retain all ordinary declarations. Only explicitly used generic definitions
        // are specialized; every resulting body passes the ordinary semantic checker.
        const auto ordinaryCount = program_.declarations.size();
        for (std::size_t index = 0; index < ordinaryCount; ++index) {
            auto declaration = std::move(program_.declarations[index]);
            currentModule_ = declaration.module;
            DiagnosticOriginScope origin(declaration.originPath);
            for (Field& field : fieldsAndConstants(declaration)) {
                normalizeType(field.type, {}, field.offset);
                normalizeExpression(field.initializer, declaration.name, {});
            }
            for (auto& method : declaration.methods)
                normalizeMethod(method, declaration.name, {});
            program_.declarations[index] = std::move(declaration);
        }
        for (auto& [owner, methods] : generatedMethods_) {
            const auto found = std::find_if(program_.declarations.begin(), program_.declarations.end(),
                [&](const Declaration& declaration) { return declaration.name == owner; });
            if (found == program_.declarations.end())
                fail("internal generic method owner was lost", 0);
            for (auto& method : methods)
                found->methods.push_back(std::move(method));
            if (found->fields.size() + found->constants.size() + found->methods.size() > 256) {
                DiagnosticOriginScope origin(found->originPath);
                fail("closed class member limit exceeded", found->offset, found->module);
            }
        }
        generatedMethods_.clear();
        resolveClassHierarchy();
        flattenSourceInterfaces();
        const bool needsDiscovery = hasGenericMembers_ && (!program_.projectEnabled ||
            std::any_of(program_.declarations.begin(), program_.declarations.end(),
                [](const Declaration& declaration) { return genericMember(declaration); }));
        if (checkSemantics && needsDiscovery) {
            checkGenericMembers();
        } else {
            lowerFacets(program_);
            if (checkSemantics)
                check(program_);
        }
    }

private:
    static bool genericMember(const Declaration& declaration) {
        const auto containsCall = [](const Field& field) { return genericMember(field.initializer); };
        if (std::any_of(declaration.fields.begin(), declaration.fields.end(), containsCall) ||
            std::any_of(declaration.constants.begin(), declaration.constants.end(), containsCall))
            return true;

        for (const auto& method : declaration.methods) {
            if (genericMember(method.baseConstructor) || genericMember(method.body))
                return true;
            if (std::any_of(method.fieldInitializers.begin(), method.fieldInitializers.end(),
                    [](const Expr& initializer) { return genericMember(initializer); }))
                return true;
        }

        return false;
    }

    static bool genericMember(const Expr& expression) {
        if (!expression)
            return false;
        if (expression->kind == "generic_member")
            return true;
        return std::any_of(expression->children.begin(), expression->children.end(),
            [](const Expr& child) { return genericMember(child); });
    }

    static bool genericMember(const std::vector<Statement>& statements) {
        return std::any_of(statements.begin(), statements.end(), [](const Statement& statement) {
            return genericMember(statement.expression) || genericMember(statement.target) ||
                genericMember(statement.initialization) || genericMember(statement.body) ||
                genericMember(statement.iteration) || genericMember(statement.alternative) ||
                genericMember(statement.cleanup);
        });
    }

    struct MemberDemand {
        std::vector<std::string> owners;
        std::string name;
        std::vector<Type> arguments;
        std::size_t offset;
        std::string module;
        std::string path;
    };

    void claimDeclaration(const Declaration& declaration) {
        DiagnosticOriginScope origin(declaration.originPath);
        budget_.projectDeclaration(declaration);
    }

    void lowerFacets(Program& target) {
        lowerProjectModelFacets(target, [&](const Declaration& declaration) {
            claimDeclaration(declaration);
        });
    }

    // Keep pre-facet declarations as the source of truth. Each discovery trial
    // owns a deep AST clone, so an interrupted checker cannot retain conversions,
    // implicit receivers, call edges, constants or partially checked bodies.
    // Every retry must add a specialization under the existing method/node caps.
    void checkGenericMembers() {
        for (;;) {
            for (const auto& declaration : program_.declarations)
                claimDeclaration(declaration);
            Program trial = program_;
            for (auto& declaration : trial.declarations)
                declaration = cloneDeclaration(declaration);
            lowerFacets(trial);
            std::map<std::string, std::string> sourceOwners;
            for (const auto& declaration : trial.declarations) {
                if (!declaration.sourceObjectFacet.empty())
                    sourceOwners.emplace(declaration.sourceObjectFacet, declaration.name);
            }
            const auto sourceName = [&](const std::string& name) {
                const auto found = sourceOwners.find(name);
                return found == sourceOwners.end() ? name : found->second;
            };
            const auto sourceType = [&](const Type& original, const auto& recurse) -> Type {
                Type result = original;
                result.name = sourceName(result.name);
                for (auto& argument : result.arguments)
                    argument = recurse(argument, recurse);
                return result;
            };
            GenericMemberResolver resolver = [&](const std::string& receiver,
                const std::string& name, const std::vector<Type>& arguments,
                const Declaration& caller, std::size_t offset) {
                DiagnosticOriginScope origin(caller.originPath);
                ModuleDiagnosticScope moduleScope(currentModule_, caller.module);
                std::vector<Type> sourceArguments;
                for (const auto& argument : arguments) {
                    budget_.type(argument, offset, currentModule_);
                    sourceArguments.push_back(sourceType(argument, sourceType));
                }
                const auto ownerName = sourceName(receiver);
                const auto found = std::find_if(program_.declarations.begin(), program_.declarations.end(),
                    [&](const Declaration& declaration) { return declaration.name == ownerName; });
                if (found == program_.declarations.end())
                    fail("unknown generic member receiver", offset, currentModule_);
                std::vector<std::string> candidates{found->name};
                candidates.insert(candidates.end(), found->ancestors.begin(), found->ancestors.end());
                std::vector<std::string> missing;
                std::string result;
                bool named = false;
                for (const auto& candidate : candidates) {
                    const auto owner = templates_.find(candidate);
                    if (owner == templates_.end() || !owner->second.contains(name))
                        continue;
                    named = true;
                    const auto& definitions = owner->second.at(name);
                    if (std::none_of(definitions.begin(), definitions.end(), [&](const Method& method) {
                        return method.typeParameters.size() == sourceArguments.size();
                    }))
                        continue;
                    const auto key = candidate + '.' + appliedName(name, sourceArguments);
                    if (const auto instance = methodInstances_.find(key); instance != methodInstances_.end())
                        result = instance->second;
                    else
                        missing.push_back(candidate);
                }
                if (!missing.empty())
                    throw MemberDemand{std::move(missing), name, std::move(sourceArguments),
                        offset, caller.module, caller.originPath};
                if (result.empty())
                    fail(named ? "explicit generic argument count mismatch" : "unknown generic method '" + name + "'",
                        offset, currentModule_);
                return result;
            };
            try {
                check(trial, resolver);
                program_ = std::move(trial);
                return;
            } catch (const MemberDemand& demand) {
                DiagnosticOriginScope origin(demand.path);
                ModuleDiagnosticScope moduleScope(currentModule_, demand.module);
                for (const auto& owner : demand.owners)
                    specializeMethods(owner, demand.name, demand.arguments, demand.offset);
                appendGeneratedMethods();
            }
        }
    }

    void appendGeneratedMethods() {
        for (auto& [name, methods] : generatedMethods_) {
            const auto owner = std::find_if(program_.declarations.begin(), program_.declarations.end(),
                [&](const Declaration& declaration) { return declaration.name == name; });
            if (owner == program_.declarations.end())
                fail("internal generic method owner was lost", 0);
            for (auto& method : methods)
                owner->methods.push_back(std::move(method));
        }
        generatedMethods_.clear();
        for (const auto& owner : program_.declarations) {
            std::size_t members = owner.fields.size() + owner.constants.size() + owner.methods.size();
            for (const auto& ancestor : owner.ancestors) {
                const auto base = std::find_if(program_.declarations.begin(), program_.declarations.end(),
                    [&](const Declaration& declaration) { return declaration.name == ancestor; });
                members += base->fields.size() + base->constants.size() + base->methods.size();
            }
            if (members > 256) {
                DiagnosticOriginScope origin(owner.originPath);
                fail("closed class inherited member limit exceeded", owner.offset, owner.module);
            }
        }
    }

    static std::string interfaceSlot(const Method& method) {
        std::string result = method.name + '(';
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            if (index != 0)
                result += ',';
            result += method.parameters[index].type.text();
        }
        return result + ')';
    }

    static bool sameInterfaceContract(const Method& left, const Method& right) {
        return left.result == right.result && left.pure == right.pure &&
            left.asynchronous == right.asynchronous;
    }

    void resolveClassHierarchy() {
        std::map<std::string, Declaration*> declarations;
        for (auto& declaration : program_.declarations)
            declarations.emplace(declaration.name, &declaration);
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (declaration.kind != "Class")
                continue;
            std::vector<Type> interfaces;
            for (const auto& base : declaration.interfaces) {
                const auto found = declarations.find(base.name);
                if (found != declarations.end() && found->second->kind == "Class") {
                    if (!program_.objectEnabled || !base.arguments.empty() || !declaration.baseClass.empty())
                        fail("class inheritance requires v3 and one nongeneric base class", declaration.offset, declaration.module);
                    if (!interfaces.empty())
                        fail("the base class must precede source interfaces", declaration.offset, declaration.module);
                    if (!program_.projectEnabled && (declaration.modelExported || found->second->modelExported))
                        fail("exported model roots cannot participate in class inheritance", declaration.offset, declaration.module);
                    declaration.baseClass = base.name;
                } else {
                    interfaces.push_back(base);
                }
            }
            declaration.interfaces = std::move(interfaces);
            for (Field& field : fieldsAndConstants(declaration))
                field.declaringClass = declaration.name;
        }
        std::map<std::string, unsigned> states;
        std::size_t copies = 0;
        std::size_t identityBytes = 0;
        std::function<void(Declaration&, unsigned)> visit = [&](Declaration& owner, unsigned depth) {
            DiagnosticOriginScope origin(owner.originPath);
            if (depth > 32)
                fail("class inheritance depth limit exceeded", owner.offset, owner.module);
            if (states[owner.name] == 1)
                fail("class inheritance cycle", owner.offset, owner.module);
            if (states[owner.name] == 2) {
                if (owner.ancestors.size() + depth > 32)
                    fail("class inheritance depth limit exceeded", owner.offset, owner.module);
                return;
            }
            states[owner.name] = 1;
            if (!owner.baseClass.empty()) {
                auto& base = *declarations.at(owner.baseClass);
                visit(base, depth + 1);
                owner.ancestors.push_back(base.name);
                owner.ancestors.insert(owner.ancestors.end(), base.ancestors.begin(), base.ancestors.end());
                for (const auto& interface : base.interfaces) {
                    if (std::find(owner.interfaces.begin(), owner.interfaces.end(), interface) == owner.interfaces.end())
                        owner.interfaces.push_back(interface);
                }
                for (const auto& ancestorName : owner.ancestors) {
                    const auto& ancestor = *declarations.at(ancestorName);
                    for (const Field& field : fieldsAndConstants(owner)) {
                        if (std::any_of(ancestor.fields.begin(), ancestor.fields.end(), [&](const Field& inherited) {
                            return inherited.name == field.name;
                        }) || std::any_of(ancestor.methods.begin(), ancestor.methods.end(), [&](const Method& inherited) {
                            return !inherited.constructor && inherited.name == field.name;
                        }))
                            fail("inherited field or method names cannot be hidden by fields", field.offset, owner.module);
                    }
                    for (const auto& method : owner.methods) {
                        if (!method.constructor && std::any_of(ancestor.fields.begin(), ancestor.fields.end(), [&](const Field& field) {
                            return field.name == method.name;
                        }))
                            fail("inherited fields cannot be hidden by methods", method.offset, owner.module);
                    }
                }
            }
            std::size_t members = owner.fields.size() + owner.constants.size() + owner.methods.size();
            for (const auto& ancestor : owner.ancestors) {
                const auto& base = *declarations.at(ancestor);
                members += base.fields.size() + base.constants.size() + base.methods.size();
                if (++copies > 65536 || ancestor.size() > messageLimit - identityBytes)
                    fail("class inheritance metadata expansion limit exceeded", owner.offset, owner.module);
                identityBytes += ancestor.size();
            }
            if (members > 256)
                fail("class inherited member limit exceeded", owner.offset, owner.module);
            states[owner.name] = 2;
        };
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (declaration.kind == "Class")
                visit(declaration, 1);
        }
    }

    void flattenSourceInterfaces() {
        if (!program_.objectEnabled)
            return;
        const bool inheritance = std::any_of(program_.declarations.begin(), program_.declarations.end(),
            [](const Declaration& declaration) { return !declaration.interfaces.empty(); });
        if (!inheritance)
            return;

        std::map<std::string, Declaration*> declarations;
        for (auto& declaration : program_.declarations)
            declarations.emplace(declaration.name, &declaration);
        std::map<std::string, unsigned> states;
        std::map<std::string, unsigned> depths;
        std::size_t methodCopies = 0;
        std::size_t identityBytes = 0;
        auto baseDeclaration = [&](const Type& base, const Declaration& owner) -> Declaration& {
            DiagnosticOriginScope origin(owner.originPath);
            const auto found = declarations.find(base.name);
            if (!base.arguments.empty() || found == declarations.end() || !found->second->sourceInterface)
                fail("inheritance requires an existing nongeneric source interface", owner.offset, owner.module);
            return *found->second;
        };
        auto claimMethod = [&](const Declaration& owner, const std::string& slot, const Method& method) {
            DiagnosticOriginScope origin(owner.originPath);
            const auto bytes = owner.name.size() + 1 + slot.size();
            if (methodCopies == 65536 || bytes > messageLimit - identityBytes)
                fail("source interface inheritance method or identity expansion limit exceeded", owner.offset, owner.module);
            budget_.method(method, owner.offset, owner.module);
            ++methodCopies;
            identityBytes += bytes;
        };
        std::function<void(Declaration&, unsigned)> flatten = [&](Declaration& owner, unsigned activeDepth) {
            DiagnosticOriginScope origin(owner.originPath);
            if (activeDepth > 32)
                fail("source interface inheritance depth limit exceeded", owner.offset, owner.module);
            if (states[owner.name] == 1)
                fail("source interface inheritance cycle", owner.offset, owner.module);
            if (states[owner.name] == 2) {
                if (depths.at(owner.name) > 33 - activeDepth)
                    fail("source interface inheritance depth limit exceeded", owner.offset, owner.module);
                return;
            }
            states[owner.name] = 1;
            std::set<std::string> direct;
            for (const auto& base : owner.interfaces) {
                auto& declaration = baseDeclaration(base, owner);
                if (!direct.insert(declaration.name).second)
                    fail("duplicate authored source interface base", owner.offset, owner.module);
            }
            std::set<std::string> authoredSlots;
            for (const auto& method : owner.methods) {
                if (!method.visible || method.constructor || !method.body.empty())
                    fail("source interfaces require public abstract methods", method.offset, owner.module);
                if (!authoredSlots.insert(interfaceSlot(method)).second)
                    fail("duplicate authored source interface method signature", method.offset, owner.module);
            }

            // Keep authored direct-base order, then append unseen ancestors.
            std::vector<Type> closure = owner.interfaces;
            std::set<std::string> ancestors = direct;
            unsigned depth = 1;
            for (const auto& base : owner.interfaces) {
                auto& declaration = baseDeclaration(base, owner);
                flatten(declaration, activeDepth + 1);
                depth = std::max(depth, depths.at(declaration.name) + 1);
                if (depth > 32)
                    fail("source interface inheritance depth limit exceeded", owner.offset, owner.module);
                for (const auto& ancestor : declaration.interfaces) {
                    if (ancestors.insert(ancestor.name).second)
                        closure.push_back(ancestor);
                }
            }

            std::vector<const Method*> effective;
            std::map<std::string, std::size_t> slots;
            for (const auto& base : owner.interfaces) {
                const auto& declaration = baseDeclaration(base, owner);
                for (const auto& method : declaration.methods) {
                    const auto slot = interfaceSlot(method);
                    const auto found = slots.find(slot);
                    if (found != slots.end()) {
                        if (!sameInterfaceContract(*effective[found->second], method))
                            fail("conflicting inherited source interface method contract", owner.offset, owner.module);
                        continue;
                    }
                    if (effective.size() == 256)
                        fail("source interface inherited member limit exceeded", owner.offset, owner.module);
                    claimMethod(owner, slot, method);
                    slots.emplace(slot, effective.size());
                    effective.push_back(&method);
                }
            }
            for (const auto& method : owner.methods) {
                const auto slot = interfaceSlot(method);
                const auto found = slots.find(slot);
                if (found != slots.end()) {
                    if (!sameInterfaceContract(*effective[found->second], method))
                        fail("conflicting redeclared source interface method contract", method.offset, owner.module);
                    // Preserve authored parameter names/offsets for validation.
                    effective[found->second] = &method;
                } else {
                    if (effective.size() == 256)
                        fail("source interface inherited member limit exceeded", owner.offset, owner.module);
                    claimMethod(owner, slot, method);
                    slots.emplace(slot, effective.size());
                    effective.push_back(&method);
                }
            }
            std::vector<Method> methods;
            methods.reserve(effective.size());
            for (const auto* method : effective)
                methods.push_back(cloneMethod(*method));
            owner.methods = std::move(methods);
            owner.interfaces = std::move(closure);
            depths[owner.name] = depth;
            states[owner.name] = 2;
        };
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (declaration.sourceInterface)
                flatten(declaration, 1);
        }
        for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (declaration.kind != "Class")
                continue;
            std::vector<Type> closure = declaration.interfaces;
            std::set<std::string> interfaces;
            for (const auto& interface : declaration.interfaces) {
                auto& contract = baseDeclaration(interface, declaration);
                if (!interfaces.insert(contract.name).second)
                    fail("duplicate authored class source interface", declaration.offset, declaration.module);
            }
            for (const auto& interface : declaration.interfaces) {
                const auto& contract = baseDeclaration(interface, declaration);
                for (const auto& ancestor : contract.interfaces) {
                    if (interfaces.insert(ancestor.name).second)
                        closure.push_back(ancestor);
                }
            }
            declaration.interfaces = std::move(closure);
        }
    }

    void resolveModelExports() {
        for (const auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
            if (!program_.objectEnabled && (declaration.sourceInterface || !declaration.interfaces.empty()))
                fail("source interfaces and object implementations require pipelang.native-model.v3",
                     declaration.offset, declaration.module);
        }
        if (!program_.objectEnabled)
            return;
        if ((!program_.projectEnabled && program_.modelExports.empty()) ||
            program_.modelExports.size() > resourceBudget().declarations)
            fail("object compilation requires 1..128 explicit model roots", 0, program_.module);
        std::vector<std::string> resolved;
        std::set<std::string> selected;
        for (const auto& requested : program_.modelExports) {
            const auto canonical = !program_.module.empty() && requested.find('.') == std::string::npos
                ? program_.module + '.' + requested : requested;
            Declaration* found = nullptr;
            for (auto& declaration : program_.declarations) {
            DiagnosticOriginScope origin(declaration.originPath);
                if ((!declaration.sourceName.empty() && declaration.sourceName == canonical) ||
                    (declaration.sourceName.empty() && declaration.name == requested)) {
                    found = &declaration;
                    break;
                }
            }
            if (!found || found->kind != "Class" || !selected.insert(found->name).second)
                fail("unknown, duplicate or non-class exported model root '" + requested + "'", 0, program_.module);
            found->modelExported = true;
            resolved.push_back(found->name);
        }
        program_.modelExports = std::move(resolved);
    }

    static void validateParameters(const std::vector<std::string>& parameters, std::size_t offset,
                                   const std::string& module) {
        if (parameters.size() > 32)
            fail("generic parameter limit exceeded", offset, module);
        std::set<std::string> names;
        for (const auto& parameter : parameters) {
            if (builtin(parameter) || !names.insert(parameter).second)
                fail("duplicate or reserved generic parameter", offset, module);
        }
    }

    Substitutions bind(const std::vector<std::string>& parameters, const std::vector<Type>& arguments,
                       std::size_t offset) {
        if (parameters.size() != arguments.size())
            fail("explicit generic argument count mismatch", offset, currentModule_);
        Substitutions substitutions;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            budget_.type(arguments[index], offset, currentModule_);
            substitutions.emplace(parameters[index], arguments[index]);
        }
        return substitutions;
    }

    void normalizeType(Type& type, const Substitutions& substitutions, std::size_t offset,
                       unsigned depth = 0) {
        if (type.name.empty())
            return;
        if (depth > 12)
            fail("closed type nesting limit exceeded", offset, currentModule_);
        if (const auto found = substitutions.find(type.name); found != substitutions.end()) {
            if (!type.arguments.empty())
                fail("a type parameter cannot receive type arguments", offset, currentModule_);
            budget_.type(found->second, offset, currentModule_, depth);
            type = found->second;
            return;
        }
        for (auto& argument : type.arguments)
            normalizeType(argument, substitutions, offset, depth + 1);
        const auto found = records_.find(type.name);
        if (found == records_.end())
            return;
        const auto& record = found->second;
        const auto& arguments = type.arguments;
        const auto canonical = appliedName(authoredName(record), arguments);
        if (const auto existing = recordInstances_.find(canonical); existing != recordInstances_.end()) {
            type = {existing->second, {}, canonical};
            return;
        }
        const auto bindings = bind(record.typeParameters, arguments, offset);
        if (activeRecords_.contains(record.name))
            fail("expanding recursive generic record instantiation", offset, currentModule_);
        if (recordInstances_.size() >= resourceBudget().genericInstances ||
            program_.declarations.size() >= resourceBudget().declarations)
            fail("closed generic type limit exceeded", offset, currentModule_);
        const auto nativeName = "pl_generic_record_" + std::to_string(recordInstances_.size());
        recordInstances_[canonical] = nativeName;
        activeRecords_.insert(record.name);
        budget_.record(record, offset, currentModule_);
        Declaration specialized = record;
        specialized.name = nativeName;
        specialized.sourceName = canonical;
        if (program_.projectEnabled) {
            specialized.genericSourceName = authoredName(record);
            specialized.genericArguments = arguments;
        }
        specialized.typeParameters.clear();
        {
            struct ModuleScope {
                std::string& target;
                std::string previous;
                ModuleScope(std::string& current, const std::string& module)
                    : target(current), previous(current) { target = module; }
                ~ModuleScope() { target = std::move(previous); }
            };
            ModuleScope moduleScope(currentModule_, record.module);
            DiagnosticOriginScope origin(record.originPath);
            for (Field& field : fieldsAndConstants(specialized)) {
                field.initializer = cloneExpression(field.initializer);
                normalizeType(field.type, bindings, field.offset, depth + 1);
                normalizeExpression(field.initializer, {}, bindings);
            }
        }
        activeRecords_.erase(record.name);
        program_.declarations.push_back(std::move(specialized));
        type = {nativeName, {}, canonical};
    }

    void normalizeMethod(Method& method, const std::string& owner, const Substitutions& substitutions) {
        const auto& [module, path] = declarationOrigins_.at(owner);
        ModuleDiagnosticScope moduleScope(currentModule_, program_.projectEnabled ? module : currentModule_);
        DiagnosticOriginScope origin(path);
        normalizeType(method.result, substitutions, method.offset);
        for (auto& parameter : method.parameters)
            normalizeType(parameter.type, substitutions, parameter.offset);
        for (auto& initializer : method.fieldInitializers)
            normalizeExpression(initializer, owner, substitutions);
        normalizeExpression(method.baseConstructor, owner, substitutions);
        normalizeStatements(method.body, owner, substitutions);
    }

    std::string specializeMethods(const std::string& owner, const std::string& name,
                                  const std::vector<Type>& arguments, std::size_t offset) {
        const auto ownerTemplates = templates_.find(owner);
        if (ownerTemplates == templates_.end() || !ownerTemplates->second.contains(name))
            fail("unknown generic method '" + name + "'", offset, currentModule_);
        const auto key = owner + '.' + appliedName(name, arguments);
        if (methodInstances_.contains(key))
            return methodInstances_.at(key);
        const auto activeKey = owner + '.' + name;
        if (activeMethods_.contains(activeKey))
            fail("expanding recursive generic method instantiation", offset, currentModule_);
        std::string nativeName;
        if (!hasGenericMembers_) {
            nativeName = "pl_generic_method_" + std::to_string(methodInstances_.size());
        } else {
            // An application has one spelling across the inheritance tree.
            // Ordinary effective-method selection then preserves signature
            // hiding, inherited overloads and the declaring class's access.
            const auto application = appliedName(name, arguments);
            auto spelling = methodNames_.find(application);
            if (spelling == methodNames_.end()) {
                const auto spellingName = "pl_generic_method_" + std::to_string(methodNames_.size());
                spelling = methodNames_.emplace(application, spellingName).first;
            }
            nativeName = spelling->second;
        }
        methodInstances_[key] = nativeName;
        activeMethods_.insert(activeKey);
        bool matched = false;
        for (const auto& definition : ownerTemplates->second.at(name)) {
            if (definition.typeParameters.size() != arguments.size())
                continue;
            matched = true;
            if (++methodCount_ > 256)
                fail("closed generic method limit exceeded", offset, currentModule_);
            budget_.method(definition, offset, currentModule_);
            auto method = cloneMethod(definition);
            method.name = nativeName;
            method.sourceName = appliedName(name, arguments);
            method.typeParameters.clear();
            normalizeMethod(method, owner, bind(definition.typeParameters, arguments, offset));
            generatedMethods_[owner].push_back(std::move(method));
        }
        activeMethods_.erase(activeKey);
        if (!matched)
            fail("explicit generic argument count mismatch", offset, currentModule_);
        return nativeName;
    }

    void planDirectStaticApplication(Expr& expression) {
        if (expression->kind != "call" || expression->children.empty())
            return;

        auto& target = expression->children.front();
        if (target->kind != "generic_member" || target->children.size() != 1)
            return;

        const auto& receiver = target->children.front();
        if (receiver->kind != "name" || !receiver->projectTypeReference ||
            !directProjectClasses_.contains(receiver->text))
            return;

        const auto owner = templates_.find(receiver->text);
        if (owner == templates_.end())
            return;

        const auto named = owner->second.find(target->text);
        if (named == owner->second.end())
            return;

        bool staticApplication = false;
        for (const auto& candidate : named->second) {
            if (candidate.typeParameters.size() != target->typeArguments.size())
                continue;
            if (!candidate.staticMethod)
                return;

            staticApplication = true;
        }
        if (!staticApplication)
            return;

        // The linker already proved the receiver despite lexical shadowing.
        // Charge the same argument and specialization bounds, then let the
        // ordinary checker resolve accessibility, overloads and returns.
        for (const auto& argument : target->typeArguments)
            budget_.type(argument, target->offset, currentModule_);

        target->text = specializeMethods(receiver->text, target->text, target->typeArguments, target->offset);
        target->typeArguments.clear();
        target->kind = "member";
    }

    void normalizeExpression(Expr& expression, const std::string& owner,
                             const Substitutions& substitutions) {
        if (!expression)
            return;
        for (auto& argument : expression->typeArguments)
            normalizeType(argument, substitutions, expression->offset);
        if (expression->kind == "type") {
            normalizeType(expression->type, substitutions, expression->offset);
            expression->text = expression->type.text();
        } else if (expression->kind == "new_construct") {
            normalizeType(expression->type, substitutions, expression->offset);
            expression->text = expression->type.name;
            expression->children[0]->text = expression->type.name;
        } else if (expression->kind == "named_construct" || expression->kind == "generic_name") {
            if (expression->kind == "named_construct") {
                if (expression->type.name.empty())
                    for (const auto& argument : expression->typeArguments)
                        budget_.type(argument, expression->offset, currentModule_);
                if (expression->type.name.empty())
                    expression->type = {expression->text, expression->typeArguments};
                normalizeType(expression->type, substitutions, expression->offset);
                expression->text = expression->type.name;
                expression->typeArguments.clear();
            }
            if (records_.contains(expression->text)) {
                for (const auto& argument : expression->typeArguments)
                    budget_.type(argument, expression->offset, currentModule_);
                Type type{expression->text, expression->typeArguments};
                normalizeType(type, substitutions, expression->offset);
                expression->text = type.name;
                expression->type = std::move(type);
                if (expression->kind == "generic_name")
                    expression->kind = "name";
            } else if (expression->kind == "generic_name") {
                expression->text = specializeMethods(owner, expression->text,
                                                     expression->typeArguments, expression->offset);
                expression->kind = "name";
            } else if (!expression->typeArguments.empty()) {
                fail("type arguments require a generic record", expression->offset, currentModule_);
            }
            expression->typeArguments.clear();
        } else if (expression->kind == "generic_member") {
            hasGenericMembers_ = true;
        }
        for (auto& child : expression->children)
            normalizeExpression(child, owner, substitutions);

        if (program_.projectEnabled)
            planDirectStaticApplication(expression);
    }

    void normalizeStatements(std::vector<Statement>& statements, const std::string& owner,
                             const Substitutions& substitutions) {
        for (auto& statement : statements) {
            if (statement.type.name.empty() && substitutions.contains("var") &&
                (statement.kind == "local" || statement.kind == "foreach" || statement.kind == "parallel_foreach"))
                statement.type = {"var", {}};

            normalizeType(statement.type, substitutions, statement.offset);
            normalizeExpression(statement.expression, owner, substitutions);
            normalizeExpression(statement.target, owner, substitutions);
            normalizeStatements(statement.initialization, owner, substitutions);
            normalizeStatements(statement.body, owner, substitutions);
            normalizeStatements(statement.iteration, owner, substitutions);
            normalizeStatements(statement.alternative, owner, substitutions);
            normalizeStatements(statement.cleanup, owner, substitutions);
        }
    }

    Program& program_;
    SpecializationBudget budget_;
    std::map<std::string, Declaration> records_;
    std::map<std::string, std::map<std::string, std::vector<Method>>> templates_;
    std::map<std::string, std::string> recordInstances_;
    std::map<std::string, std::string> methodInstances_;
    std::map<std::string, std::string> methodNames_;
    std::map<std::string, std::vector<Method>> generatedMethods_;
    std::set<std::string> directProjectClasses_;
    std::set<std::string> activeRecords_;
    std::set<std::string> activeMethods_;
    std::map<std::string, std::pair<std::string, std::string>> declarationOrigins_;
    std::string currentModule_;
    std::size_t methodCount_ = 0;
    bool hasGenericMembers_ = false;
};

}

Program linkModules(const std::vector<ModuleSource>& sources, const std::string& entry, bool objectSyntax, bool projectSyntax) {
    return ModuleLinker().run(sources, entry, objectSyntax, projectSyntax);
}

Declaration cloneDeclaration(const Declaration& declaration) {
    auto result = declaration;
    for (auto& field : result.fields) {
        field.initializer = cloneExpression(field.initializer);
        field.constantValue = cloneExpression(field.constantValue);
    }
    for (auto& constant : result.constants) {
        constant.initializer = cloneExpression(constant.initializer);
        constant.constantValue = cloneExpression(constant.constantValue);
    }
    result.methods.clear();
    for (const auto& method : declaration.methods)
        result.methods.push_back(cloneMethod(method));
    return result;
}

void normalizeComposition(Program& program) {
    CompositionNormalizer(program).run();
}

void normalizeAndCheckComposition(Program& program) {
    CompositionNormalizer(program).run(true);
}

CheckedProgram::CheckedProgram(Program program)
    : program_(std::move(program))
{
}

CheckedProgram CheckedProgram::complete(Program program, bool enableObjects,
                                        std::vector<std::string> modelExports)
{
    program.objectEnabled = enableObjects || program.projectEnabled;
    program.modelExports = std::move(modelExports);
    normalizeAndCheckComposition(program);

    return CheckedProgram(std::move(program));
}

CheckedProgram CheckedProgram::project(const std::vector<ModuleSource>& modules,
                                       const std::string& entry, bool activeHostNumericEnvironment)
{
    auto program = linkModules(modules, entry, true, true);
    program.activeHostNumericEnvironment = activeHostNumericEnvironment;

    std::vector<std::string> modelExports;
    for (const auto& declaration : program.declarations) {
        if (declaration.modelDeclared)
            modelExports.push_back(declaration.sourceName);
    }

    return complete(std::move(program), true, std::move(modelExports));
}

}
