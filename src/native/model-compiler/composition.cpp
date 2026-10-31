// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "composition.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace pipelang::models {
namespace {

using Substitutions = std::map<std::string, Type>;

[[noreturn]] void fail(const std::string& message, std::size_t offset,
                       const std::string& module = {}) {
    Diagnostic diagnostic(message, offset);
    diagnostic.module = module;
    throw diagnostic;
}

bool builtin(const std::string& name) {
    static const std::set<std::string> names = {
        "void", "bool", "int", "float", "float32", "string", "Error", "CancellationToken",
        "Result", "Task", "Optional", "List", "Atomic", "Array", "Dictionary",
        "HashSet", "Queue", "Stack", "KeyValuePair",
        "ConcurrentDictionary", "ConcurrentQueue", "ConcurrentStack", "Lock", "SemaphoreSlim"
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
        for (const auto& child : value.body)
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
    }

private:
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
    result->children.clear();
    for (const auto& child : original->children)
        result->children.push_back(cloneExpression(child));
    return result;
}

Statement cloneStatement(const Statement& original) {
    Statement result = original;
    result.expression = cloneExpression(original.expression);
    result.target = cloneExpression(original.target);
    result.body.clear();
    result.alternative.clear();
    result.cleanup.clear();
    for (auto& capture : result.parallelCaptures)
        capture.initializer = cloneExpression(capture.initializer);
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
    result.fieldInitializers.clear();
    for (const auto& initializer : original.fieldInitializers)
        result.fieldInitializers.push_back(cloneExpression(initializer));
    for (auto& parameter : result.parameters)
        parameter.initializer = cloneExpression(parameter.initializer);
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

class ModuleLinker {
public:
    Program run(const std::vector<ModuleSource>& sources, const std::string& entry, bool objectSyntax) {
        if (sources.empty() || sources.size() > 128 || !moduleName(entry))
            fail("closed module graph requires 1..128 modules and a valid entry", 0);
        std::size_t sourceBytes = 0;
        std::size_t declarationCount = 0;
        std::size_t tokenCount = 0;
        std::size_t expressionCount = 0;
        for (const auto& source : sources) {
            if (!moduleName(source.name) || programs_.contains(source.name))
                fail("invalid or duplicate supplied module '" + source.name + "'", 0, source.name);
            if (source.source.size() > sourceLimit - sourceBytes)
                fail("aggregate module source byte limit exceeded", 0, source.name);
            sourceBytes += source.source.size();
            Program program;
            try {
                program = parse(source.source, objectSyntax);
            } catch (Diagnostic& diagnostic) {
                diagnostic.module = source.name;
                throw;
            }
            if (program.module != source.name)
                fail("source module declaration must match its supplied module identity", 0, source.name);
            if (program.tokenCount > 32768 - tokenCount)
                fail("aggregate module token limit exceeded", 0, source.name);
            tokenCount += program.tokenCount;
            if (program.expressionCount > 16384 - expressionCount)
                fail("aggregate module expression node limit exceeded", 0, source.name);
            expressionCount += program.expressionCount;
            declarationCount += program.declarations.size();
            if (declarationCount > 128)
                fail("aggregate module declaration limit exceeded", 0, source.name);
            programs_.emplace(source.name, std::move(program));
        }
        if (!programs_.contains(entry))
            fail("entry module was not supplied", 0, entry);
        for (const auto& [name, program] : programs_) {
            std::set<std::string> imported;
            for (const auto& dependency : program.imports) {
                if (!programs_.contains(dependency))
                    fail("imported module was not supplied: " + dependency, 0, name);
                if (dependency == name || !imported.insert(dependency).second)
                    fail("self or duplicate module import", 0, name);
            }
        }
        std::set<std::string> visiting;
        std::set<std::string> visited;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            if (visiting.contains(name))
                fail("module import cycle", 0, name);
            if (visited.contains(name))
                return;
            visiting.insert(name);
            for (const auto& dependency : programs_.at(name).imports)
                visit(dependency);
            visiting.erase(name);
            visited.insert(name);
        };
        // Validate every supplied module, including modules outside the entry closure.
        for (const auto& [name, program] : programs_)
            visit(name);
        std::set<std::string> reachable;
        std::function<void(const std::string&)> reach = [&](const std::string& name) {
            if (!reachable.insert(name).second)
                return;
            for (const auto& dependency : programs_.at(name).imports)
                reach(dependency);
        };
        reach(entry);
        if (reachable.size() != programs_.size())
            fail("supplied module is outside the entry import closure", 0, entry);

        std::size_t identity = 0;
        for (auto& [name, program] : programs_) {
            std::set<std::string> localNames;
            for (auto& declaration : program.declarations) {
                if (!localNames.insert(declaration.name).second || builtin(declaration.name))
                    fail("duplicate or reserved module type", declaration.offset, name);
                const auto canonical = name + '.' + declaration.name;
                // Preserve the entry class names used by native host consumers.
                names_[canonical] = name == entry ? declaration.name
                    : "pl_module_type_" + std::to_string(identity++);
            }
        }
        const auto inheritedNames = objectSyntax ? inheritedClassNames() : std::map<std::string, std::set<std::string>>{};
        Program linked;
        linked.module = entry;
        linked.tokenCount = tokenCount;
        linked.expressionCount = expressionCount;
        for (auto& [name, program] : programs_) {
            current_ = &program;
            for (auto& declaration : program.declarations) {
                const auto canonical = name + '.' + declaration.name;
                declaration.sourceName = canonical;
                declaration.module = name;
                declaration.name = names_.at(canonical);
                std::set<std::string> parameters(declaration.typeParameters.begin(),
                                                 declaration.typeParameters.end());
                for (auto& interface : declaration.interfaces)
                    rewriteType(interface, parameters, declaration.offset);
                std::set<std::string> values;
                for (const auto& field : declaration.fields)
                    values.insert(field.name);
                if (const auto inherited = inheritedNames.find(canonical); inherited != inheritedNames.end())
                    values.insert(inherited->second.begin(), inherited->second.end());
                for (auto& field : declaration.fields) {
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
                if (declaration.kind != "Class")
                    continue;
                const auto canonical = module + '.' + declaration.name;
                classes.emplace(canonical, &declaration);
                for (const auto& field : declaration.fields)
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
                        if (members[owner].size() == 256)
                            fail("class inherited member name limit exceeded", classes.at(owner)->offset, classes.at(owner)->module);
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

    std::string resolve(const std::string& name, std::size_t offset, bool required) const {
        const auto local = current_->module + '.' + name;
        if (names_.contains(local))
            return local;
        std::vector<std::string> candidates;
        for (const auto& dependency : current_->imports) {
            const auto candidate = dependency + '.' + name;
            if (names_.contains(candidate))
                candidates.push_back(candidate);
        }
        if (candidates.size() > 1)
            fail("ambiguous imported type '" + name + "'", offset, current_->module);
        if (candidates.size() == 1)
            return candidates.front();
        if (names_.contains(name)) {
            const auto separator = name.rfind('.');
            const auto owner = name.substr(0, separator);
            if (owner == current_->module ||
                std::find(current_->imports.begin(), current_->imports.end(), owner) != current_->imports.end())
                return name;
            fail("qualified type requires an explicit direct import", offset, current_->module);
        }
        if (required)
            fail("unknown or unimported type '" + name + "'", offset, current_->module);
        return {};
    }

    void rewriteType(Type& type, const std::set<std::string>& parameters, std::size_t offset) {
        if (type.name.empty())
            return;
        for (auto& argument : type.arguments)
            rewriteType(argument, parameters, offset);
        if (builtin(type.name) || parameters.contains(type.name))
            return;
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
            rewriteType(statement.type, parameters, statement.offset);
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

    std::map<std::string, Program> programs_;
    std::map<std::string, std::string> names_;
    Program* current_ = nullptr;
};

class CompositionNormalizer {
public:
    explicit CompositionNormalizer(Program& program) : program_(program) {}

    void run() {
        resolveModelExports();
        // Store templates separately so vector growth never invalidates their identity.
        std::vector<Declaration> concrete;
        for (auto& declaration : program_.declarations) {
            validateParameters(declaration.typeParameters, declaration.offset, declaration.module);
            if (!declaration.typeParameters.empty()) {
                if (declaration.kind != "Record")
                    fail("user generic declarations currently require Record", declaration.offset, declaration.module);
                if (!declaration.methods.empty())
                    fail("generic records declare owned value fields only", declaration.offset, declaration.module);
                std::set<std::string> fieldNames;
                for (const auto& field : declaration.fields) {
                    if (!field.visible || !fieldNames.insert(field.name).second)
                        fail("generic record fields must be public and unique", field.offset, declaration.module);
                }
                const auto name = declaration.name;
                const auto offset = declaration.offset;
                const auto module = declaration.module;
                if (!records_.emplace(name, std::move(declaration)).second)
                    fail("duplicate generic record", offset, module);
            } else {
                concrete.push_back(std::move(declaration));
            }
        }
        program_.declarations = std::move(concrete);
        for (auto& declaration : program_.declarations) {
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
            for (auto& field : declaration.fields) {
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
            if (found->fields.size() + found->methods.size() > 256)
                fail("closed class member limit exceeded", found->offset, found->module);
        }
        resolveClassHierarchy();
        flattenSourceInterfaces();
    }

private:
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
                    if (declaration.modelExported || found->second->modelExported)
                        fail("exported model roots cannot participate in class inheritance", declaration.offset, declaration.module);
                    declaration.baseClass = base.name;
                } else {
                    interfaces.push_back(base);
                }
            }
            declaration.interfaces = std::move(interfaces);
            for (auto& field : declaration.fields)
                field.declaringClass = declaration.name;
        }
        std::map<std::string, unsigned> states;
        std::size_t copies = 0;
        std::size_t identityBytes = 0;
        std::function<void(Declaration&, unsigned)> visit = [&](Declaration& owner, unsigned depth) {
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
                    for (const auto& field : owner.fields) {
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
            std::size_t members = owner.fields.size() + owner.methods.size();
            for (const auto& ancestor : owner.ancestors) {
                const auto& base = *declarations.at(ancestor);
                members += base.fields.size() + base.methods.size();
                if (++copies > 65536 || ancestor.size() > messageLimit - identityBytes)
                    fail("class inheritance metadata expansion limit exceeded", owner.offset, owner.module);
                identityBytes += ancestor.size();
            }
            if (members > 256)
                fail("class inherited member limit exceeded", owner.offset, owner.module);
            states[owner.name] = 2;
        };
        for (auto& declaration : program_.declarations) {
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
            const auto found = declarations.find(base.name);
            if (!base.arguments.empty() || found == declarations.end() || !found->second->sourceInterface)
                fail("inheritance requires an existing nongeneric source interface", owner.offset, owner.module);
            return *found->second;
        };
        auto claimMethod = [&](const Declaration& owner, const std::string& slot, const Method& method) {
            const auto bytes = owner.name.size() + 1 + slot.size();
            if (methodCopies == 65536 || bytes > messageLimit - identityBytes)
                fail("source interface inheritance method or identity expansion limit exceeded", owner.offset, owner.module);
            budget_.method(method, owner.offset, owner.module);
            ++methodCopies;
            identityBytes += bytes;
        };
        std::function<void(Declaration&, unsigned)> flatten = [&](Declaration& owner, unsigned activeDepth) {
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
            if (declaration.sourceInterface)
                flatten(declaration, 1);
        }
        for (auto& declaration : program_.declarations) {
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
            if (!program_.objectEnabled && (declaration.sourceInterface || !declaration.interfaces.empty()))
                fail("source interfaces and object implementations require pipelang.native-model.v3",
                     declaration.offset, declaration.module);
        }
        if (!program_.objectEnabled)
            return;
        if (program_.modelExports.empty() || program_.modelExports.size() > 128)
            fail("object compilation requires 1..128 explicit model roots", 0, program_.module);
        std::vector<std::string> resolved;
        std::set<std::string> selected;
        for (const auto& requested : program_.modelExports) {
            const auto canonical = !program_.module.empty() && requested.find('.') == std::string::npos
                ? program_.module + '.' + requested : requested;
            Declaration* found = nullptr;
            for (auto& declaration : program_.declarations) {
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
        if (recordInstances_.size() >= 128 || program_.declarations.size() >= 128)
            fail("closed generic type limit exceeded", offset, currentModule_);
        const auto nativeName = "pl_generic_record_" + std::to_string(recordInstances_.size());
        recordInstances_[canonical] = nativeName;
        activeRecords_.insert(record.name);
        budget_.record(record, offset, currentModule_);
        Declaration specialized = record;
        specialized.name = nativeName;
        specialized.sourceName = canonical;
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
            for (auto& field : specialized.fields) {
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
        const auto nativeName = "pl_generic_method_" + std::to_string(methodInstances_.size());
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
            fail("generic instance member calls are unsupported; use a local generic method", expression->offset,
                 currentModule_);
        }
        for (auto& child : expression->children)
            normalizeExpression(child, owner, substitutions);
    }

    void normalizeStatements(std::vector<Statement>& statements, const std::string& owner,
                             const Substitutions& substitutions) {
        for (auto& statement : statements) {
            normalizeType(statement.type, substitutions, statement.offset);
            normalizeExpression(statement.expression, owner, substitutions);
            normalizeExpression(statement.target, owner, substitutions);
            normalizeStatements(statement.body, owner, substitutions);
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
    std::map<std::string, std::vector<Method>> generatedMethods_;
    std::set<std::string> activeRecords_;
    std::set<std::string> activeMethods_;
    std::string currentModule_;
    std::size_t methodCount_ = 0;
};

}

Program linkModules(const std::vector<ModuleSource>& sources, const std::string& entry, bool objectSyntax) {
    return ModuleLinker().run(sources, entry, objectSyntax);
}

void normalizeComposition(Program& program) {
    CompositionNormalizer(program).run();
}

}
