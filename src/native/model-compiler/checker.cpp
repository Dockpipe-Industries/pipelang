// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>

namespace pipelang::models {
namespace {

const Type booleanType{"bool", {}};
const Type integerType{"int", {}};
const Type stringType{"string", {}};
const Type voidType{"void", {}};

class Checker {
public:
    explicit Checker(Program& program) : program_(program) {}
    void run() {
        for (auto& declaration : program_.declarations) {
            if (types_.contains(declaration.name) || primitive(declaration.name) ||
                declaration.name == "Result" || declaration.name == "Task" ||
                declaration.name == "Optional" || declaration.name == "CancellationToken")
                fail("duplicate or reserved type '" + declaration.name + "'", declaration.offset);
            types_[declaration.name] = &declaration;
        }
        for (auto& declaration : program_.declarations)
            signatures(declaration);
        checkRecordCycles();
        for (auto& declaration : program_.declarations) {
            if (declaration.kind != "Record")
                continue;
            owner_ = &declaration;
            Method initializerMethod;
            initializerMethod.pure = true;
            method_ = &initializerMethod;
            locals_.clear();
            for (auto& field : declaration.fields) {
                if (field.initializer) {
                    same(expression(field.initializer), field.type, field.offset);
                    checkConstantInitializer(field.initializer);
                }
            }
        }
        for (auto& declaration : program_.declarations) {
            if (declaration.kind != "Class")
                continue;
            owner_ = &declaration;
            for (auto& method : declaration.methods) {
                method_ = &method;
                locals_.clear();
                for (const auto& parameter : method.parameters)
                    locals_[parameter.name] = parameter.type;
                if (method.constructor) {
                    for (auto& field : declaration.fields) {
                        if (field.initializer)
                            same(expression(field.initializer), field.type, field.offset);
                    }
                }
                const bool returns = statements(method.body);
                const Type expected = method.asynchronous ? method.result.arguments[0] : method.result;
                if (expected != voidType && !returns)
                    fail("method must return on every path", method.offset);
            }
            checkCallCycles(declaration);
        }
    }
private:
    [[noreturn]] static void fail(const std::string& message, std::size_t offset) {
        throw Diagnostic(message, offset);
    }
    static bool primitive(const std::string& name) {
        return name == "void" || name == "bool" || name == "int" || name == "string";
    }
    Declaration* declaration(const std::string& name) const {
        auto found = types_.find(name);
        return found == types_.end() ? nullptr : found->second;
    }
    void type(const Type& value, std::size_t offset, bool allowVoid = false, bool allowTask = false) {
        if (value.name == "void") {
            if (!allowVoid || !value.arguments.empty())
                fail("void is not a value type", offset);
            return;
        }
        if (value.name == "Result") {
            if (value.arguments.size() != 2)
                fail("Result requires value and error types", offset);
            type(value.arguments[0], offset, true);
            type(value.arguments[1], offset);
            if (value.arguments[0].name != "void" && !plainValue(value.arguments[0]))
                fail("Result payloads must recursively contain owned values", offset);
            if (value.arguments[1] != stringType)
                fail("this native capability requires string Result errors", offset);
            return;
        }
        if (value.name == "Task") {
            if (!allowTask || value.arguments.size() != 1 || value.arguments[0].name != "Result")
                fail("Task is supported only as Task<Result<T,string>> method return", offset);
            type(value.arguments[0], offset);
            return;
        }
        if (!value.arguments.empty())
            fail("unsupported generic type '" + value.text() + "'", offset);
        if (primitive(value.name) || value.name == "CancellationToken")
            return;
        const auto* found = declaration(value.name);
        if (!found || found->kind == "Class")
            fail("unknown value or service type '" + value.text() + "'", offset);
    }
    bool service(const Type& value) const {
        const auto* found = declaration(value.name);
        return found && found->kind == "Interface";
    }
    bool plainValue(const Type& value) const {
        if (service(value) || value.name == "Task" || value.name == "CancellationToken" || value.name == "void")
            return false;
        for (const auto& argument : value.arguments) {
            if (argument.name != "void" && !plainValue(argument))
                return false;
        }
        return true;
    }
    void signatures(Declaration& owner) {
        std::set<std::string> members;
        for (const auto& value : owner.values) {
            if (!members.insert(value).second)
                fail("duplicate enum value", owner.offset);
        }
        if (owner.kind == "Enum" && owner.values.empty())
            fail("empty enum is not supported", owner.offset);
        for (const auto& field : owner.fields) {
            type(field.type, field.offset);
            if (!members.insert(field.name).second)
                fail("duplicate member '" + field.name + "'", field.offset);
            if (field.initializer && service(field.type))
                fail("service fields do not have initializers", field.offset);
            if (owner.kind == "Record" && !field.visible)
                fail("record fields must be public", field.offset);
            if (owner.kind == "Record" && !plainValue(field.type))
                fail("records contain owned value fields only", field.offset);
            if (service(field.type) && field.visible)
                fail("host service fields must be private", field.offset);
            if (field.type.name == "CancellationToken")
                fail("cancellation tokens are method parameters, not model state", field.offset);
        }
        std::size_t constructors = 0;
        for (const auto& method : owner.methods) {
            if (owner.kind == "Record")
                fail("records declare fields; positional constructors are generated", method.offset);
            if (!members.insert(method.name).second)
                fail("duplicate method; overloading is not supported", method.offset);
            if (method.constructor) {
                ++constructors;
                if (!method.visible || method.pure || method.asynchronous || owner.kind != "Class")
                    fail("invalid constructor", method.offset);
            } else {
                type(method.result, method.offset, true, method.asynchronous);
                if (method.asynchronous != (method.result.name == "Task"))
                    fail("async method requires Task<Result<T,string>>", method.offset);
                if (method.pure && (method.asynchronous || method.result == voidType))
                    fail("pure method requires a synchronous value return", method.offset);
                if (service(method.result) || method.result.name == "CancellationToken")
                    fail("methods cannot return service references or cancellation tokens", method.offset);
                if (owner.kind == "Interface" && !method.asynchronous)
                    fail("host interfaces declare Task<Result<T,string>> methods", method.offset);
            }
            std::set<std::string> parameters;
            for (const auto& parameter : method.parameters) {
                type(parameter.type, parameter.offset);
                if (!parameters.insert(parameter.name).second)
                    fail("duplicate parameter", parameter.offset);
                if (service(parameter.type) && !method.constructor)
                    fail("service references enter through constructors only", parameter.offset);
                if (std::any_of(owner.fields.begin(), owner.fields.end(), [&](const Field& field) {
                    return field.name == parameter.name;
                }))
                    fail("parameters must not shadow model fields", parameter.offset);
            }
        }
        if (owner.kind == "Class" && constructors == 0) {
            Method constructor;
            constructor.name = owner.name;
            constructor.constructor = true;
            constructor.result = voidType;
            constructor.offset = owner.offset;
            owner.methods.push_back(std::move(constructor));
        }
    }
    const Field* field(const Declaration& owner, const std::string& name) const {
        const auto found = std::find_if(owner.fields.begin(), owner.fields.end(), [&](const Field& value) {
            return value.name == name;
        });
        return found == owner.fields.end() ? nullptr : &*found;
    }
    Method* method(const std::string& name) const {
        const auto found = std::find_if(owner_->methods.begin(), owner_->methods.end(), [&](const Method& value) {
            return value.name == name;
        });
        return found == owner_->methods.end() ? nullptr : &*found;
    }
    void same(const Type& actual, const Type& expected, std::size_t offset) {
        if (actual != expected)
            fail("expected " + expected.text() + ", got " + actual.text(), offset);
    }
    Type expression(const Expr& node, bool allowAwait = false, bool allowServiceCall = false) {
        Type result;
        if (node->kind == "integer") {
            std::int64_t parsed = 0;
            const auto conversion = std::from_chars(node->text.data(), node->text.data() + node->text.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != node->text.data() + node->text.size())
                fail("integer literal outside signed64 range", node->offset);
            result = integerType;
        } else if (node->kind == "text") {
            result = stringType;
        } else if (node->kind == "boolean") {
            result = booleanType;
        } else if (node->kind == "name") {
            if (auto local = locals_.find(node->text); local != locals_.end()) {
                result = local->second;
                node->resolution = "local";
            } else if (const auto* found = field(*owner_, node->text)) {
                result = found->type;
                node->resolution = service(result) ? "service" : "field";
            } else {
                fail("unknown value '" + node->text + "'", node->offset);
            }
        } else if (node->kind == "unary") {
            // The minimum signed integer has no positive int64 counterpart.
            if (node->text == "-" && node->children[0]->kind == "integer" &&
                node->children[0]->text == "9223372036854775808") {
                node->resolution = "minimum_int";
                result = integerType;
            } else {
                result = expression(node->children[0]);
                same(result, node->text == "!" ? booleanType : integerType, node->offset);
            }
        } else if (node->kind == "binary") {
            const auto left = expression(node->children[0]);
            const auto right = expression(node->children[1]);
            same(right, left, node->offset);
            const auto& operation = node->text;
            if (operation == "&&" || operation == "||") {
                same(left, booleanType, node->offset);
                result = booleanType;
            } else if (operation == "==" || operation == "!=") {
                if (!plainValue(left))
                    fail("equality requires owned values", node->offset);
                result = booleanType;
            } else if (operation == "+" && left == stringType) {
                result = stringType;
            } else {
                same(left, integerType, node->offset);
                result = operation == "+" || operation == "-" || operation == "*" || operation == "/"
                    ? integerType : booleanType;
            }
        } else if (node->kind == "conditional") {
            same(expression(node->children[0]), booleanType, node->children[0]->offset);
            result = expression(node->children[1]);
            same(expression(node->children[2]), result, node->children[2]->offset);
            if (!plainValue(result))
                fail("conditional arms must have the same owned value type", node->offset);
        } else if (node->kind == "member") {
            const auto& receiver = node->children[0];
            const auto* named = receiver->kind == "name" ? declaration(receiver->text) : nullptr;
            if (named && named->kind == "Enum") {
                if (std::find(named->values.begin(), named->values.end(), node->text) == named->values.end())
                    fail("unknown enum value", node->offset);
                result = {named->name, {}};
                node->resolution = "enum";
            } else {
                const auto receiverType = expression(receiver);
                if (receiverType.name == "Result") {
                    if (node->text == "IsSuccess" || node->text == "IsFailure" || node->text == "IsCancelled")
                        result = booleanType;
                    else if (node->text == "Error")
                        result = receiverType.arguments[1];
                    else if (node->text == "Value" && receiverType.arguments[0] != voidType)
                        result = receiverType.arguments[0];
                    else
                        fail("unknown Result property", node->offset);
                    node->resolution = "result";
                } else if (receiverType.name == "CancellationToken" && node->text == "IsCancellationRequested") {
                    result = booleanType;
                    node->resolution = "cancellation";
                } else {
                    const auto* record = declaration(receiverType.name);
                    const auto* found = record && record->kind == "Record" ? field(*record, node->text) : nullptr;
                    if (!found || !found->visible)
                        fail("unknown record field", node->offset);
                    result = found->type;
                    node->resolution = "record";
                }
            }
        } else if (node->kind == "named_construct") {
            const auto* record = declaration(node->text);
            if (!record || record->kind != "Record")
                fail("named construction requires a record type", node->offset);
            std::set<std::string> assigned;
            for (const auto& child : node->children) {
                const auto* target = field(*record, child->text);
                if (!target || !assigned.insert(child->text).second)
                    fail("unknown or duplicate record initializer", child->offset);
                same(expression(child->children[0]), target->type, child->offset);
            }
            result = {record->name, {}};
        } else if (node->kind == "call") {
            result = call(node, allowServiceCall);
        } else if (node->kind == "await") {
            if (!allowAwait || !method_->asynchronous)
                fail("await must be a complete local initializer or assignment in an async method", node->offset);
            result = expression(node->children[0], false, true);
            if (result.name != "Task")
                fail("await requires a host Task<Result<T,string>> call", node->offset);
            const auto awaited = result.arguments[0];
            result = awaited;
        } else {
            fail("type names are not values", node->offset);
        }
        node->type = result;
        return result;
    }
    void arguments(const Expr& node, const std::vector<Field>& parameters) {
        if (node->children.size() != parameters.size() + 1)
            fail("argument count mismatch", node->offset);
        for (std::size_t index = 0; index < parameters.size(); ++index)
            same(expression(node->children[index + 1]), parameters[index].type, node->children[index + 1]->offset);
    }
    Type call(const Expr& node, bool allowServiceCall) {
        const auto& target = node->children[0];
        if (target->kind == "name") {
            const auto* record = declaration(target->text);
            if (record && record->kind == "Record") {
                arguments(node, record->fields);
                node->resolution = "construct";
                return {record->name, {}};
            }
            const auto* found = method(target->text);
            if (!found || found->constructor || found->asynchronous)
                fail("local calls require synchronous methods; local Task composition is not admitted", target->offset);
            if (method_->pure && !found->pure)
                fail("pure methods cannot call mutating helpers", target->offset);
            arguments(node, found->parameters);
            node->resolution = found->pure ? "pure" : "effect";
            method_->calls.insert(found->name);
            return found->result;
        }
        if (target->kind != "member")
            fail("unsupported call target", node->offset);
        const auto& receiver = target->children[0];
        if (receiver->kind == "type" && receiver->type.name == "Result") {
            type(receiver->type, receiver->offset);
            const auto& result = receiver->type;
            std::vector<Field> parameters;
            if (target->text == "Success") {
                if (result.arguments[0] != voidType)
                    parameters.push_back({result.arguments[0], "value", true, node->offset});
            } else if (target->text == "Failure" || target->text == "Cancelled") {
                if (target->text == "Failure" || node->children.size() != 1)
                    parameters.push_back({result.arguments[1], "error", true, node->offset});
            } else {
                fail("unknown Result constructor", node->offset);
            }
            arguments(node, parameters);
            node->resolution = "result_construct";
            return result;
        }
        const auto receiverType = expression(receiver);
        const auto* host = declaration(receiverType.name);
        if (!host || host->kind != "Interface" || !allowServiceCall || method_->pure)
            fail("host calls must be directly awaited in an async method", node->offset);
        const auto found = std::find_if(host->methods.begin(), host->methods.end(), [&](const Method& method) {
            return method.name == target->text;
        });
        if (found == host->methods.end())
            fail("unknown host service method", target->offset);
        arguments(node, found->parameters);
        node->resolution = "host";
        return found->result;
    }
    void assignmentTarget(const Expr& target) {
        expression(target);
        Expr root = target;
        while (root->kind == "member") {
            if (root->resolution != "record")
                fail("assignment requires mutable record field", root->offset);
            root = root->children[0];
        }
        if (root->kind != "name" || (root->resolution != "field" && root->resolution != "local" && root->resolution != "service"))
            fail("invalid assignment target", root->offset);
        if (method_->pure && root->resolution != "local")
            fail("pure methods cannot mutate model fields", root->offset);
        if (root->resolution == "service" && !method_->constructor)
            fail("service identity is fixed by the constructor", root->offset);
    }
    bool statements(std::vector<Statement>& body) {
        bool returned = false;
        for (auto& statement : body) {
            if (returned)
                fail("unreachable statement", statement.offset);
            if (statement.kind == "local") {
                type(statement.type, statement.offset);
                if (!plainValue(statement.type) || locals_.contains(statement.name) || field(*owner_, statement.name))
                    fail("local must be an owned value with a unique unshadowed name", statement.offset);
                same(expression(statement.expression, true), statement.type, statement.offset);
                locals_[statement.name] = statement.type;
            } else if (statement.kind == "expression") {
                if (statement.expression->kind != "call")
                    fail("expression statements require a method call", statement.offset);
                same(expression(statement.expression), voidType, statement.offset);
            } else if (statement.kind == "assign") {
                assignmentTarget(statement.target);
                same(expression(statement.expression, true), statement.target->type, statement.offset);
            } else if (statement.kind == "return") {
                if (method_->constructor)
                    fail("constructor returns are not supported; initialize all services and fields", statement.offset);
                const Type expected = method_->asynchronous ? method_->result.arguments[0] : method_->result;
                const auto actual = statement.expression ? expression(statement.expression) : voidType;
                same(actual, expected, statement.offset);
                returned = true;
            } else if (statement.kind == "if") {
                same(expression(statement.expression), booleanType, statement.offset);
                auto outer = locals_;
                const bool first = statements(statement.body);
                locals_ = outer;
                const bool second = statements(statement.alternative);
                locals_ = std::move(outer);
                returned = first && second;
            }
        }
        return returned;
    }
    void checkConstantInitializer(const Expr& node) {
        if (node->resolution == "field" || node->resolution == "service" ||
            node->resolution == "local" || node->resolution == "pure" || node->resolution == "effect")
            fail("record defaults must be independent value expressions", node->offset);
        for (const auto& child : node->children)
            checkConstantInitializer(child);
    }
    void checkRecordCycles() {
        struct Layout {
            std::size_t leaves = 1;
            unsigned depth = 1;
        };
        constexpr std::size_t maximumLeaves = 256;
        std::set<std::string> visiting;
        std::map<std::string, Layout> layouts;
        std::function<Layout(const Type&, std::size_t)> visitType;
        std::function<Layout(const Declaration&)> visitRecord;
        auto merge = [&](Layout& target, const Layout& child, std::size_t offset) {
            if (child.leaves > maximumLeaves || target.leaves > maximumLeaves - child.leaves)
                fail("expanded value layout limit exceeded", offset);
            target.leaves += child.leaves;
            target.depth = std::max(target.depth, child.depth + 1);
            if (target.depth > 32)
                fail("value layout depth limit exceeded", offset);
        };
        visitType = [&](const Type& type, std::size_t offset) -> Layout {
            const auto* found = declaration(type.name);
            if (found && found->kind == "Record")
                return visitRecord(*found);
            Layout layout;
            for (const auto& argument : type.arguments) {
                if (argument.name != "void")
                    merge(layout, visitType(argument, offset), offset);
            }
            return layout;
        };
        visitRecord = [&](const Declaration& record) -> Layout {
            if (visiting.contains(record.name))
                fail("recursive record value graph is not supported", record.offset);
            if (auto found = layouts.find(record.name); found != layouts.end())
                return found->second;
            visiting.insert(record.name);
            Layout layout;
            for (const auto& field : record.fields)
                merge(layout, visitType(field.type, field.offset), field.offset);
            visiting.erase(record.name);
            layouts[record.name] = layout;
            return layout;
        };
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind == "Record") {
                visitRecord(declaration);
            } else if (declaration.kind == "Class") {
                Layout layout;
                for (const auto& field : declaration.fields) {
                    if (!service(field.type))
                        merge(layout, visitType(field.type, field.offset), field.offset);
                }
            }
        }
    }
    void checkCallCycles(const Declaration& owner) {
        struct Cost {
            std::size_t work = 0;
            unsigned depth = 0;
        };
        constexpr std::size_t maximumWork = 65536;
        std::set<std::string> visiting;
        std::map<std::string, Cost> costs;
        std::function<Cost(const Method&)> visit;
        std::function<std::size_t(const Expr&)> expressionCost;
        std::function<std::size_t(const std::vector<Statement>&)> statementCost;
        auto addCost = [&](std::size_t left, std::size_t right, std::size_t offset) {
            if (right > maximumWork || left > maximumWork - right)
                fail("expanded method evaluation work limit exceeded", offset);
            return left + right;
        };
        expressionCost = [&](const Expr& node) -> std::size_t {
            if (!node)
                return 0;
            std::size_t work = 1;
            for (const auto& child : node->children)
                work = addCost(work, expressionCost(child), node->offset);
            if (node->kind == "call" && (node->resolution == "pure" || node->resolution == "effect"))
                work = addCost(work, visit(*this->method(node->children[0]->text)).work, node->offset);
            return work;
        };
        statementCost = [&](const std::vector<Statement>& body) -> std::size_t {
            std::size_t work = 0;
            for (const auto& statement : body) {
                work = addCost(work, 1, statement.offset);
                work = addCost(work, expressionCost(statement.expression), statement.offset);
                work = addCost(work, expressionCost(statement.target), statement.offset);
                work = addCost(work, statementCost(statement.body), statement.offset);
                work = addCost(work, statementCost(statement.alternative), statement.offset);
            }
            return work;
        };
        visit = [&](const Method& method) -> Cost {
            if (visiting.contains(method.name))
                fail("recursive local call graph", method.offset);
            if (auto found = costs.find(method.name); found != costs.end())
                return found->second;
            visiting.insert(method.name);
            Cost cost;
            cost.depth = 1;
            for (const auto& name : method.calls)
                cost.depth = std::max(cost.depth, visit(*this->method(name)).depth + 1);
            if (cost.depth > 32)
                fail("local call depth limit exceeded", method.offset);
            cost.work = statementCost(method.body);
            if (method.constructor) {
                for (const auto& field : owner.fields)
                    cost.work = addCost(cost.work, expressionCost(field.initializer), field.offset);
            }
            visiting.erase(method.name);
            costs[method.name] = cost;
            return cost;
        };
        for (const auto& method : owner.methods)
            visit(method);
    }
    Program& program_;
    std::map<std::string, Declaration*> types_;
    Declaration* owner_ = nullptr;
    Method* method_ = nullptr;
    std::map<std::string, Type> locals_;
};

}

void check(Program& program) {
    Checker(program).run();
}

}
