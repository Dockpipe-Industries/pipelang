// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>

namespace pipelang::models {
namespace {

const Type booleanType{"bool", {}};
const Type integerType{"int", {}};
const Type floatType{"float", {}};
const Type stringType{"string", {}};
const Type voidType{"void", {}};
const Type errorType{"Error", {}};
const Type cancellationType{"CancellationToken", {}};

class Checker {
public:
    explicit Checker(Program& program) : program_(program) {}
    void run() {
        for (auto& declaration : program_.declarations) {
            owner_ = &declaration;
            if (types_.contains(declaration.name) || primitive(declaration.name) ||
                declaration.name == "Result" || declaration.name == "Task" ||
                declaration.name == "Optional" || declaration.name == "List" || declaration.name == "CancellationToken" ||
                declaration.name == "Atomic" || extended(declaration.name) || isConcurrentCollection(declaration.name) ||
                isSynchronizationHandle(declaration.name))
                fail("duplicate or reserved type '" + declaration.name + "'", declaration.offset);
            types_[declaration.name] = &declaration;
        }
        std::set<std::string> interfaceSignatures;
        std::function<void(Declaration&)> sourceSignatures = [&](Declaration& declaration) {
            if (!interfaceSignatures.insert(declaration.name).second)
                return;
            for (const auto& base : declaration.interfaces) {
                if (auto* ancestor = this->declaration(base.name); ancestor && ancestor->sourceInterface)
                    sourceSignatures(*ancestor);
            }
            owner_ = &declaration;
            signatures(declaration);
        };
        if (program_.objectEnabled) {
            for (auto& declaration : program_.declarations) {
                if (declaration.sourceInterface)
                    sourceSignatures(declaration);
            }
        }
        for (auto& declaration : program_.declarations) {
            if (program_.objectEnabled && declaration.sourceInterface)
                continue;
            owner_ = &declaration;
            signatures(declaration);
        }
        if (program_.objectEnabled) {
            for (auto& declaration : program_.declarations) {
                for (auto& method : declaration.methods)
                    methods_[method.identity] = &method;
            }
            checkClassOverrides();
            checkImplementations();
        }
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
                    sameExpression(field.initializer, field.type);
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
                    if (!declaration.baseClass.empty()) {
                        if (!method.baseConstructor) {
                            method.baseConstructor = std::make_shared<Expression>();
                            method.baseConstructor->kind = "call";
                            method.baseConstructor->offset = method.offset;
                            method.baseConstructor->children.push_back(std::make_shared<Expression>());
                        }
                        auto& base = *this->declaration(declaration.baseClass);
                        checkingBaseArguments_ = true;
                        const auto* selected = overload(base.methods, base.name, method.baseConstructor);
                        checkingBaseArguments_ = false;
                        if (!selected->visible && !selected->protectedMember)
                            fail("base constructor is private", method.offset);
                        method.baseConstructor->resolution = "object_construct";
                        method.baseConstructor->callable = selected->identity;
                        method.baseConstructor->owner = base.name;
                        singleTarget(method.baseConstructor, selected->identity);
                        trackCalls(method.baseConstructor->targets);
                    } else if (method.baseConstructor) {
                        fail("base initializer requires a base class", method.offset);
                    }
                    method.fieldInitializers.clear();
                    for (const auto& field : declaration.fields) {
                        auto initializer = cloneInitializer(field.initializer);
                        if (initializer)
                            sameExpression(initializer, field.type);
                        method.fieldInitializers.push_back(std::move(initializer));
                    }
                }
                const auto flow = statements(method.body);
                const Type expected = method.asynchronous ? method.result.arguments[0] : method.result;
                if (expected != voidType && (flow & Fallthrough) != 0)
                    fail("method must return on every path", method.offset);
            }
            if (!program_.objectEnabled) {
                checkCallCycles(declaration);
                checkWorkerCalls(declaration);
                checkSynchronizationCalls(declaration);
            }
        }
        if (program_.objectEnabled) {
            for (auto& declaration : program_.declarations) {
                if (declaration.kind != "Class")
                    continue;
                owner_ = &declaration;
                checkCallCycles(declaration);
                checkWorkerCalls(declaration);
                checkSynchronizationCalls(declaration);
            }
        }
    }
private:
    struct OwnerContext {
        Declaration*& slot;
        Declaration* previous;
        OwnerContext(Declaration*& current, Declaration* replacement)
            : slot(current), previous(current) { slot = replacement; }
        ~OwnerContext() { slot = previous; }
    };
    Declaration* methodOwner(const Method& method) const {
        if (!program_.objectEnabled)
            return owner_;
        const auto separator = method.identity.find('.');
        const auto found = types_.find(method.identity.substr(0, separator));
        return found == types_.end() ? owner_ : found->second;
    }
    [[noreturn]] void fail(const std::string& message, std::size_t offset) const {
        Diagnostic diagnostic(message, offset);
        if (owner_)
            diagnostic.module = owner_->module;
        throw diagnostic;
    }
    static bool extended(const std::string& name) {
        return name == "Array" || name == "Dictionary" || name == "HashSet" ||
            name == "Queue" || name == "Stack" || name == "KeyValuePair";
    }
    bool keyType(const Type& value) const {
        const auto* found = declaration(value.name);
        return value.arguments.empty() && (value.name == "int" || value.name == "bool" ||
            value.name == "string" || (found && found->kind == "Enum"));
    }
    Type nominal(const Declaration& value) const {
        return {value.name, {}, value.sourceName};
    }
    bool objectClass(const Declaration& declaration) const {
        return program_.objectEnabled && declaration.kind == "Class" && !declaration.modelExported;
    }
    bool objectReference(const Type& value) const {
        const auto* found = declaration(value.name);
        return found && (objectClass(*found) || (program_.objectEnabled && found->sourceInterface));
    }
    bool referenceBearing(const Type& value) const {
        if (objectReference(value))
            return true;
        return std::any_of(value.arguments.begin(), value.arguments.end(),
            [&](const Type& argument) { return referenceBearing(argument); });
    }
    bool sourceValue(const Type& value) const {
        if (objectReference(value))
            return true;
        if (service(value) || isSharedHandle(value) || value.name == "Task" ||
            value.name == "CancellationToken" || value.name == "void")
            return false;
        return std::all_of(value.arguments.begin(), value.arguments.end(), [&](const Type& argument) {
            return argument.name == "void" || sourceValue(argument);
        });
    }
    bool assignable(const Type& actual, const Type& expected) const {
        if (actual == expected)
            return true;
        const auto* concrete = declaration(actual.name);
        const auto* contract = declaration(expected.name);
        if (concrete && contract && objectClass(*concrete) && objectClass(*contract))
            return std::find(concrete->ancestors.begin(), concrete->ancestors.end(), contract->name) != concrete->ancestors.end();
        return concrete && contract && (objectClass(*concrete) || concrete->sourceInterface) && contract->sourceInterface &&
            std::any_of(concrete->interfaces.begin(), concrete->interfaces.end(),
                [&](const Type& implemented) { return implemented == expected; });
    }
    Type sequenceItem(const Type& sequence, std::size_t offset) const {
        if (isConcurrentCollection(sequence.name) && method_->pure)
            fail("pure methods cannot observe concurrent collection state", offset);
        if (sequence.name == "Dictionary" || sequence.name == "ConcurrentDictionary")
            return {"KeyValuePair", sequence.arguments};
        if (sequence.name == "List" || sequence.name == "Array" || sequence.name == "HashSet" ||
            sequence.name == "Queue" || sequence.name == "Stack" ||
            sequence.name == "ConcurrentQueue" || sequence.name == "ConcurrentStack")
            return sequence.arguments.at(0);
        fail("foreach requires an owned sequence collection", offset);
    }
    Expr cloneInitializer(const Expr& original) {
        if (!original)
            return {};
        if (++initializerNodes_ > 65536)
            fail("constructor initializer expansion limit exceeded", original->offset);
        auto result = std::make_shared<Expression>(*original);
        result->children.clear();
        for (const auto& child : original->children)
            result->children.push_back(cloneInitializer(child));
        return result;
    }
    static bool primitive(const std::string& name) {
        return name == "void" || name == "bool" || name == "int" || name == "float" || name == "string" || name == "Error";
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
        if (isSynchronizationHandle(value.name)) {
            if (!value.arguments.empty())
                fail("Lock and SemaphoreSlim do not accept type arguments", offset);
            return;
        }
        if (isConcurrentCollection(value.name)) {
            const auto arity = value.name == "ConcurrentDictionary" ? 2U : 1U;
            if (value.arguments.size() != arity)
                fail("concurrent collection generic argument count mismatch", offset);
            for (const auto& argument : value.arguments) {
                type(argument, offset);
                if (!plainValue(argument))
                    fail("concurrent collections require owned value payloads", offset);
            }
            if (value.name == "ConcurrentDictionary" && !keyType(value.arguments[0]))
                fail("concurrent dictionary requires int, bool, string or enum keys", offset);
            return;
        }
        if (value.name == "Atomic") {
            if (value.arguments.size() != 1 ||
                (value.arguments[0] != integerType && value.arguments[0] != booleanType && value.arguments[0] != floatType))
                fail("Atomic requires int, bool or float", offset);
            return;
        }
        if (extended(value.name)) {
            const std::size_t arity = value.name == "Dictionary" || value.name == "KeyValuePair" ? 2 : 1;
            if (value.arguments.size() != arity)
                fail("collection generic argument count mismatch", offset);
            for (const auto& argument : value.arguments) {
                type(argument, offset);
                if (!sourceValue(argument))
                    fail("collections require owned value payloads", offset);
            }
            if ((value.name == "Dictionary" || value.name == "HashSet") && !keyType(value.arguments[0]))
                fail("hashed collections require int, bool, string or enum keys", offset);
            return;
        }
        if (value.name == "List") {
            if (value.arguments.size() != 1)
                fail("List requires one owned value type", offset);
            type(value.arguments[0], offset);
            if (!sourceValue(value.arguments[0]))
                fail("List requires an owned value payload", offset);
            return;
        }
        if (value.name == "Optional") {
            if (value.arguments.size() != 1)
                fail("Optional requires one owned value type", offset);
            type(value.arguments[0], offset);
            if (!sourceValue(value.arguments[0]))
                fail("Optional requires an owned value payload", offset);
            return;
        }
        if (value.name == "Result") {
            if (value.arguments.size() != 2)
                fail("Result requires value and error types", offset);
            type(value.arguments[0], offset, true);
            type(value.arguments[1], offset);
            if (value.arguments[0].name != "void" && !sourceValue(value.arguments[0]))
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
        if (!found || (found->kind == "Class" && !objectClass(*found)) ||
            (found->sourceInterface && !program_.objectEnabled))
            fail("unknown value or service type '" + value.text() + "'", offset);
    }
    bool service(const Type& value) const {
        const auto* found = declaration(value.name);
        return found && found->kind == "Interface" && !found->sourceInterface;
    }
    bool plainValue(const Type& value) const {
        if (objectReference(value) || service(value) || isSharedHandle(value) || value.name == "Task" || value.name == "CancellationToken" || value.name == "void")
            return false;
        for (const auto& argument : value.arguments) {
            if (argument.name != "void" && !plainValue(argument))
                return false;
        }
        return true;
    }
    bool derivesFrom(const Declaration& owner, const std::string& ancestor) const {
        return owner.name == ancestor || std::find(owner.ancestors.begin(), owner.ancestors.end(), ancestor) != owner.ancestors.end();
    }
    bool accessible(bool visible, bool protectedMember, const Declaration& declaring) const {
        return visible || owner_->name == declaring.name || (protectedMember && derivesFrom(*owner_, declaring.name));
    }
    const Declaration& declaringMethod(const Method& method) const {
        return *methodOwner(method);
    }
    static bool sameSlot(const Method& left, const Method& right) {
        if (left.name != right.name || left.parameters.size() != right.parameters.size())
            return false;
        for (std::size_t index = 0; index < left.parameters.size(); ++index) {
            if (left.parameters[index].type != right.parameters[index].type)
                return false;
        }
        return true;
    }
    std::vector<Method*> effectiveMethods(Declaration& owner) const {
        std::vector<Method*> result;
        const auto add = [&](Declaration& declaration) {
            for (auto& method : declaration.methods) {
                if (method.constructor && declaration.name != owner.name)
                    continue;
                if (std::none_of(result.begin(), result.end(), [&](const Method* selected) { return sameSlot(*selected, method); }))
                    result.push_back(&method);
            }
        };
        add(owner);
        for (const auto& ancestor : owner.ancestors)
            add(*declaration(ancestor));
        return result;
    }
    void checkClassOverrides() {
        std::vector<Declaration*> ordered;
        for (auto& owner : program_.declarations)
            ordered.push_back(&owner);
        std::stable_sort(ordered.begin(), ordered.end(), [](const Declaration* left, const Declaration* right) {
            return left->ancestors.size() < right->ancestors.size();
        });
        for (auto* current : ordered) {
            auto& owner = *current;
            owner_ = &owner;
            for (auto& method : owner.methods) {
                const Method* inherited = nullptr;
                for (const auto& ancestor : owner.ancestors) {
                    for (const auto& candidate : declaration(ancestor)->methods) {
                        if (!candidate.constructor && sameSlot(candidate, method)) {
                            inherited = &candidate;
                            break;
                        }
                    }
                    if (inherited)
                        break;
                }
                if (method.virtualMethod || method.overrideMethod) {
                    if (!objectClass(owner) || method.constructor || (!method.visible && !method.protectedMember) || !method.typeParameters.empty())
                        fail("virtual and override require accessible nongeneric ordinary instance methods", method.offset);
                }
                if (inherited) {
                    if (!method.overrideMethod || !inherited->virtualMethod || method.virtualMethod)
                        fail("inherited method slots require an explicit override of a virtual method", method.offset);
                    if (method.result != inherited->result || method.pure != inherited->pure ||
                        method.asynchronous != inherited->asynchronous || method.visible != inherited->visible ||
                        method.protectedMember != inherited->protectedMember)
                        fail("override must preserve the exact return, visibility, pure and async contract", method.offset);
                    method.virtualMethod = true;
                } else if (method.overrideMethod) {
                    fail("override has no inherited virtual slot", method.offset);
                }
            }
        }
        // Every possible receiver contributes its effective slot to closed analysis.
        for (auto& owner : program_.declarations) {
            for (auto& method : owner.methods) {
                if (!method.virtualMethod)
                    continue;
                for (auto& concrete : program_.declarations) {
                    if (!objectClass(concrete) || !derivesFrom(concrete, owner.name))
                        continue;
                    for (auto* target : effectiveMethods(concrete)) {
                        if (sameSlot(*target, method)) {
                            claimCallIdentity(target->identity, method.offset);
                            dispatch_[method.identity].push_back(target->identity);
                            break;
                        }
                    }
                }
            }
        }
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
            if (field.protectedMember && (!objectClass(owner)))
                fail("protected fields require an ordinary source class", field.offset);
            if (!members.insert(field.name).second)
                fail("duplicate member '" + field.name + "'", field.offset);
            if (field.initializer && service(field.type))
                fail("service fields do not have initializers", field.offset);
            if (owner.kind == "Record" && !field.visible)
                fail("record fields must be public", field.offset);
            if (owner.kind == "Record" && !plainValue(field.type))
                fail("records contain owned value fields only", field.offset);
            if (program_.objectEnabled && owner.modelExported && field.visible && !plainValue(field.type))
                fail("exported model snapshot fields must recursively contain owned values", field.offset);
            if (isConcurrentCollection(field.type.name) && field.visible)
                fail("concurrent collection fields must be private and cannot enter owned snapshots", field.offset);
            if (isSynchronizationHandle(field.type.name) && field.visible)
                fail("synchronization fields must be private and cannot enter owned snapshots", field.offset);
            if (field.type.name == "Atomic" && field.visible)
                fail("Atomic fields must be private and cannot enter owned snapshots", field.offset);
            if (service(field.type) && field.visible)
                fail("host service fields must be private", field.offset);
            if (field.type.name == "CancellationToken")
                fail("cancellation tokens are method parameters, not model state", field.offset);
        }
        std::size_t constructors = 0;
        std::set<std::string> identities;
        for (auto& method : owner.methods) {
            if (owner.kind == "Record")
                fail("records declare fields; positional constructors are generated", method.offset);
            if (members.contains(method.name))
                fail("duplicate method or field name", method.offset);
            method.identity = method.name + "(";
            for (std::size_t index = 0; index < method.parameters.size(); ++index) {
                if (index != 0)
                    method.identity += ",";
                method.identity += method.parameters[index].type.text();
            }
            method.identity += ")";
            if (program_.objectEnabled)
                method.identity = owner.name + '.' + method.identity;
            if (!identities.insert(method.identity).second)
                fail("duplicate method signature; return types do not distinguish overloads", method.offset);
            if (method.protectedMember && !objectClass(owner))
                fail("protected members require an ordinary source class", method.offset);
            if (!program_.objectEnabled && (method.virtualMethod || method.overrideMethod || method.baseConstructor))
                fail("class inheritance syntax requires pipelang.native-model.v3", method.offset);
            if (owner.kind == "Interface" && !method.visible)
                fail("host interface methods must be public", method.offset);
            if (method.constructor) {
                ++constructors;
                if ((!method.visible && !objectClass(owner)) || method.pure || method.asynchronous || owner.kind != "Class")
                    fail("invalid constructor", method.offset);
            } else {
                type(method.result, method.offset, true, method.asynchronous);
                if (method.asynchronous != (method.result.name == "Task"))
                    fail("async method requires Task<Result<T,string>>", method.offset);
                if (method.pure && (method.asynchronous || method.result == voidType))
                    fail("pure method requires a synchronous value return", method.offset);
                if (isConcurrentCollection(method.result.name))
                    fail("methods cannot return concurrent collection handles", method.offset);
                if (isSynchronizationHandle(method.result.name))
                    fail("methods cannot return synchronization handles", method.offset);
                if (service(method.result) || method.result.name == "Atomic" || method.result.name == "CancellationToken")
                    fail("methods cannot return service references or cancellation tokens", method.offset);
                if (owner.kind == "Interface" && !owner.sourceInterface && !method.asynchronous)
                    fail("host interfaces declare Task<Result<T,string>> methods", method.offset);
                if (owner.kind == "Interface" && !owner.sourceInterface && referenceBearing(method.result))
                    fail("host interfaces cannot return owner-confined object references", method.offset);
            }
            std::set<std::string> parameters;
            for (const auto& parameter : method.parameters) {
                type(parameter.type, parameter.offset);
                if (!parameters.insert(parameter.name).second)
                    fail("duplicate parameter", parameter.offset);
                if (owner.kind == "Interface" && !owner.sourceInterface && isSharedHandle(parameter.type))
                    fail("host interfaces cannot transport shared handles", parameter.offset);
                if (owner.kind == "Interface" && !owner.sourceInterface && referenceBearing(parameter.type))
                    fail("host interfaces cannot transport owner-confined object references", parameter.offset);
                if (service(parameter.type) && !method.constructor)
                    fail("service references enter through constructors only", parameter.offset);
                if (field(owner, parameter.name))
                    fail("parameters must not shadow model fields", parameter.offset);
            }
        }
        if (owner.kind == "Class" && constructors == 0) {
            Method constructor;
            constructor.name = owner.name;
            constructor.constructor = true;
            constructor.result = voidType;
            constructor.offset = owner.offset;
            constructor.identity = constructor.name + "()";
            if (program_.objectEnabled)
                constructor.identity = owner.name + '.' + constructor.identity;
            owner.methods.push_back(std::move(constructor));
        }
    }
    const Field* field(const Declaration& owner, const std::string& name) const {
        const auto found = std::find_if(owner.fields.begin(), owner.fields.end(), [&](const Field& value) {
            return value.name == name;
        });
        if (found != owner.fields.end())
            return &*found;
        if (!owner.baseClass.empty())
            return field(*declaration(owner.baseClass), name);
        return nullptr;
    }
    void checkImplementations() {
        for (auto& concrete : program_.declarations) {
            if (concrete.sourceInterface)
                continue;
            owner_ = &concrete;
            if (concrete.modelExported && !concrete.interfaces.empty())
                fail("exported model roots cannot implement source interfaces", concrete.offset);
            std::set<std::string> implemented;
            for (const auto& interfaceType : concrete.interfaces) {
                type(interfaceType, concrete.offset);
                const auto* contract = declaration(interfaceType.name);
                if (concrete.kind != "Class" || !contract || !contract->sourceInterface ||
                    !implemented.insert(contract->name).second)
                    fail("classes implement distinct source interfaces only", concrete.offset);
                for (const auto& required : contract->methods) {
                    Method* found = nullptr;
                    for (auto* candidate : effectiveMethods(concrete)) {
                        if (!candidate->constructor && sameSlot(*candidate, required)) {
                            found = candidate;
                            break;
                        }
                    }
                    if (!found || !found->visible || found->result != required.result ||
                        found->pure != required.pure || found->asynchronous != required.asynchronous)
                        fail("class does not publicly implement the exact source interface contract", concrete.offset);
                    claimCallIdentity(found->identity, concrete.offset);
                    dispatch_[required.identity].push_back(found->identity);
                }
            }
        }
    }
    Method* method(const std::string& identity) const {
        if (program_.objectEnabled) {
            const auto found = methods_.find(identity);
            return found == methods_.end() ? nullptr : found->second;
        }
        const auto found = std::find_if(owner_->methods.begin(), owner_->methods.end(), [&](const Method& value) {
            return value.identity == identity;
        });
        return found == owner_->methods.end() ? nullptr : &*found;
    }
    Method* overload(std::vector<Method>& methods, const std::string& name, const Expr& node) {
        std::vector<Method*> candidates;
        for (auto& method : methods)
            candidates.push_back(&method);
        return overload(candidates, name, node);
    }
    Method* overload(Declaration& owner, const std::string& name, const Expr& node) {
        auto candidates = effectiveMethods(owner);
        return overload(candidates, name, node);
    }
    Method* overload(const std::vector<Method*>& methods, const std::string& name, const Expr& node) {
        std::vector<Type> actual;
        for (std::size_t index = 1; index < node->children.size(); ++index)
            actual.push_back(expression(node->children[index]));
        Method* selected = nullptr;
        for (auto* candidatePointer : methods) {
            auto& candidate = *candidatePointer;
            if (candidate.name != name || candidate.parameters.size() != actual.size())
                continue;
            bool matches = true;
            for (std::size_t index = 0; index < actual.size(); ++index)
                matches = matches && candidate.parameters[index].type == actual[index];
            if (!matches)
                continue;
            if (selected)
                fail("ambiguous exact method overload", node->offset);
            selected = &candidate;
        }
        if (!selected && program_.objectEnabled) {
            for (auto* candidatePointer : methods) {
                auto& candidate = *candidatePointer;
                if (candidate.name != name || candidate.parameters.size() != actual.size())
                    continue;
                bool matches = true;
                for (std::size_t index = 0; index < actual.size(); ++index)
                    matches = matches && assignable(actual[index], candidate.parameters[index].type);
                if (!matches)
                    continue;
                if (selected)
                    fail("ambiguous source interface upcast overload", node->offset);
                selected = &candidate;
            }
        }
        if (!selected)
            fail("no exact matching method overload for '" + name + "'", node->offset);
        for (std::size_t index = 0; index < actual.size(); ++index) {
            if (actual[index] != selected->parameters[index].type)
                node->children[index + 1]->convertTo = selected->parameters[index].type;
        }
        return selected;
    }
    void same(const Type& actual, const Type& expected, std::size_t offset) {
        if (!assignable(actual, expected))
            fail("expected " + expected.text() + ", got " + actual.text(), offset);
    }
    Type sameExpression(const Expr& node, const Type& expected, bool allowAwait = false) {
        const auto actual = expression(node, allowAwait);
        same(actual, expected, node->offset);
        if (actual != expected)
            node->convertTo = expected;
        return actual;
    }
    void claimCallIdentity(const std::string& identity, std::size_t offset) {
        if (!program_.objectEnabled)
            return;
        if (callEdges_ == 65536 || identity.size() > messageLimit - callIdentityBytes_)
            fail("closed object dispatch metadata limit exceeded", offset);
        ++callEdges_;
        callIdentityBytes_ += identity.size();
    }
    void singleTarget(const Expr& node, const std::string& identity) {
        claimCallIdentity(identity, node->offset);
        node->targets = {identity};
    }
    void multipleTargets(const Expr& node, const std::vector<std::string>& identities) {
        for (const auto& identity : identities)
            claimCallIdentity(identity, node->offset);
        node->targets = identities;
    }
    void trackCalls(const std::vector<std::string>& targets) {
        for (const auto& identity : targets) {
            method_->calls.insert(identity);
            if (parallelDepth_ != 0)
                method_->parallelCalls.insert(identity);
            if (finallyDepth_ != 0)
                method_->cleanupCalls.insert(identity);
            if (lockDepth_ != 0)
                method_->lockCalls.insert(identity);
        }
    }
    Expr thisExpression(std::size_t offset) {
        if (++objectNodes_ > 65536)
            fail("object receiver normalization node limit exceeded", offset);
        auto receiver = std::make_shared<Expression>();
        receiver->kind = "this";
        receiver->text = "this";
        receiver->resolution = "this";
        receiver->offset = offset;
        receiver->type = nominal(*owner_);
        receiver->owner = owner_->name;
        return receiver;
    }
    Type objectCall(const Expr& node, const Type& receiverType, bool allowServiceCall,
                    const Method* preselected = nullptr) {
        if (parallelDepth_ != 0)
            fail("owner-confined object calls cannot enter parallel workers", node->offset);
        auto* receiver = declaration(receiverType.name);
        const auto& target = node->children[0];
        const auto* selected = preselected ? preselected : overload(*receiver, target->text, node);
        const auto& declaring = declaringMethod(*selected);
        if (selected->constructor || !accessible(selected->visible, selected->protectedMember, declaring))
            fail("unknown or private object method", target->offset);
        if (method_->pure && !selected->pure)
            fail("pure methods cannot call object effects", target->offset);
        if (selected->asynchronous && (!allowServiceCall || !method_->asynchronous))
            fail("async object calls must be directly awaited in an async method", target->offset);
        node->owner = declaring.name;
        node->callable = selected->identity;
        if (receiver->sourceInterface) {
            multipleTargets(node, dispatch_[selected->identity]);
            if (node->targets.empty())
                fail("source interface call has no closed implementation targets", target->offset);
            node->resolution = selected->asynchronous ? "source_interface_async" : "source_interface_call";
        } else if (selected->virtualMethod && target->children[0]->kind != "base") {
            multipleTargets(node, dispatch_[selected->identity]);
            node->resolution = selected->asynchronous ? "class_virtual_async" : "class_virtual_call";
        } else {
            singleTarget(node, selected->identity);
            node->resolution = selected->asynchronous ? "object_async" : "object_call";
        }
        trackCalls(node->targets);
        return selected->result;
    }
    Type expression(const Expr& node, bool allowAwait = false, bool allowServiceCall = false) {
        Type result;
        if (node->kind == "integer") {
            std::int64_t parsed = 0;
            const auto conversion = std::from_chars(node->text.data(), node->text.data() + node->text.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != node->text.data() + node->text.size())
                fail("integer literal outside signed64 range", node->offset);
            result = integerType;
        } else if (node->kind == "float") {
            double parsed = 0;
            const auto conversion = std::from_chars(node->text.data(), node->text.data() + node->text.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != node->text.data() + node->text.size() || !std::isfinite(parsed))
                fail("float literal outside finite binary64 range", node->offset);
            node->resolution = std::to_string(std::bit_cast<std::uint64_t>(parsed));
            result = floatType;
        } else if (node->kind == "text") {
            result = stringType;
        } else if (node->kind == "boolean") {
            result = booleanType;
        } else if (node->kind == "base") {
            if (!objectClass(*owner_) || owner_->baseClass.empty() || checkingBaseArguments_ || parallelDepth_ != 0)
                fail("base requires an ordinary derived instance outside workers and base arguments", node->offset);
            result = nominal(*declaration(owner_->baseClass));
            node->resolution = "base";
            node->owner = owner_->baseClass;
        } else if (node->kind == "this") {
            if (checkingBaseArguments_)
                fail("base arguments cannot access the instance", node->offset);
            if (!objectClass(*owner_))
                fail("this is available only inside an ordinary source object", node->offset);
            if (parallelDepth_ != 0)
                fail("owner-confined objects cannot enter parallel workers", node->offset);
            result = nominal(*owner_);
            node->resolution = "this";
            node->owner = owner_->name;
        } else if (node->kind == "name") {
            if (auto local = locals_.find(node->text); local != locals_.end()) {
                result = local->second;
                node->resolution = "local";
                if (parallelDepth_ != 0 && parallelOuter_.contains(node->text))
                    parallelUsed_.insert(node->text);
            } else if (const auto* found = field(*owner_, node->text)) {
                if (checkingBaseArguments_ || !accessible(found->visible, found->protectedMember,
                    *declaration(found->declaringClass.empty() ? owner_->name : found->declaringClass)))
                    fail("field is private or unavailable before base construction", node->offset);
                result = found->type;
                node->resolution = objectClass(*owner_) ? "object_field" : (service(result) ? "service" : "field");
                node->owner = found->declaringClass.empty() ? owner_->name : found->declaringClass;
                if (parallelDepth_ != 0 && service(result))
                    fail("parallel workers cannot access host services", node->offset);
            } else {
                fail("unknown value '" + node->text + "'", node->offset);
            }
            if (parallelDepth_ != 0 && (node->resolution == "object_field" || referenceBearing(result)))
                fail("owner-confined object references cannot enter parallel workers", node->offset);
        } else if (node->kind == "unary") {
            // The minimum signed integer has no positive int64 counterpart.
            if (node->text == "-" && node->children[0]->kind == "integer" &&
                node->children[0]->text == "9223372036854775808") {
                node->resolution = "minimum_int";
                result = integerType;
            } else {
                result = expression(node->children[0]);
                if (node->text == "!")
                    same(result, booleanType, node->offset);
                else if (result != integerType && result != floatType)
                    fail("negation requires int or float", node->offset);
            }
        } else if (node->kind == "binary") {
            const auto left = expression(node->children[0]);
            const auto right = expression(node->children[1]);
            const bool referenceEquality = (node->text == "==" || node->text == "!=") &&
                objectReference(left) && objectReference(right);
            if (!referenceEquality)
                same(right, left, node->offset);
            const auto& operation = node->text;
            if (operation == "&&" || operation == "||") {
                same(left, booleanType, node->offset);
                result = booleanType;
            } else if (operation == "==" || operation == "!=") {
                if (!sourceValue(left))
                    fail("equality requires owned values", node->offset);
                if (referenceEquality)
                    node->resolution = "object_identity";
                result = booleanType;
            } else if (operation == "+" && left == stringType) {
                result = stringType;
            } else {
                if (left != integerType && left != floatType)
                    fail("numeric operation requires int or float", node->offset);
                result = operation == "+" || operation == "-" || operation == "*" || operation == "/"
                    ? left : booleanType;
            }
        } else if (node->kind == "conditional") {
            same(expression(node->children[0]), booleanType, node->children[0]->offset);
            result = expression(node->children[1]);
            const auto alternative = expression(node->children[2]);
            if (alternative != result && assignable(result, alternative)) {
                node->children[1]->convertTo = alternative;
                result = alternative;
            } else {
                same(alternative, result, node->children[2]->offset);
                if (alternative != result)
                    node->children[2]->convertTo = result;
            }
            if (!sourceValue(result))
                fail("conditional arms must have the same owned value type", node->offset);
        } else if (node->kind == "member") {
            const auto& receiver = node->children[0];
            const auto* named = receiver->kind == "name" ? declaration(receiver->text) : nullptr;
            if (named && named->kind == "Enum") {
                if (std::find(named->values.begin(), named->values.end(), node->text) == named->values.end())
                    fail("unknown enum value", node->offset);
                result = nominal(*named);
                node->resolution = "enum";
            } else {
                const auto receiverType = expression(receiver);
                if (objectReference(receiverType)) {
                    const auto* object = declaration(receiverType.name);
                    const auto* member = object && objectClass(*object) ? field(*object, node->text) : nullptr;
                    if (!member || !accessible(member->visible, member->protectedMember, *declaration(member->declaringClass)))
                        fail("unknown or private object field", node->offset);
                    if (parallelDepth_ != 0)
                        fail("owner-confined object fields cannot enter parallel workers", node->offset);
                    result = member->type;
                    node->resolution = "object_field";
                    node->owner = member->declaringClass;
                    node->type = result;
                    return result;
                }
                if (isConcurrentCollection(receiverType.name) && method_->pure)
                    fail("pure methods cannot observe concurrent collection state", node->offset);
                if (receiverType.name == "SemaphoreSlim") {
                    if (method_->pure)
                        fail("pure methods cannot observe semaphore state", node->offset);
                    if (node->text != "CurrentCount")
                        fail("unknown SemaphoreSlim property", node->offset);
                    result = integerType;
                    node->resolution = "sync_count";
                } else if (receiverType.name == "KeyValuePair") {
                    if (node->text != "Key" && node->text != "Value")
                        fail("unknown KeyValuePair property", node->offset);
                    result = receiverType.arguments[node->text == "Key" ? 0 : 1];
                    node->resolution = node->text == "Key" ? "pair_key" : "pair_value";
                } else if ((receiverType.name == "Dictionary" || receiverType.name == "ConcurrentDictionary") &&
                           (node->text == "Keys" || node->text == "Values" || node->text == "Entries")) {
                    result = {"List", {node->text == "Entries" ? Type{"KeyValuePair", receiverType.arguments}
                        : receiverType.arguments[node->text == "Keys" ? 0 : 1]}};
                    node->resolution = node->text == "Keys" ? "dictionary_keys" :
                        (node->text == "Values" ? "dictionary_values" : "dictionary_entries");
                } else if (receiverType == errorType) {
                    if (node->text != "Message")
                        fail("unknown Error property", node->offset);
                    result = stringType;
                    node->resolution = "error";
                } else if (receiverType.name == "List" || extended(receiverType.name) || isConcurrentCollection(receiverType.name)) {
                    if (node->text != "Count" && !(receiverType.name == "Array" && node->text == "Length"))
                        fail("unknown List property", node->offset);
                    result = integerType;
                    node->resolution = "list";
                } else if (receiverType.name == "Optional") {
                    if (node->text == "HasValue")
                        result = booleanType;
                    else if (node->text == "Value")
                        result = receiverType.arguments[0];
                    else
                        fail("unknown Optional property", node->offset);
                    node->resolution = "optional";
                } else if (receiverType.name == "Result") {
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
        } else if (node->kind == "new_construct") {
            if (!program_.objectEnabled)
                fail("object construction requires pipelang.native-model.v3", node->offset);
            if (method_->pure || parallelDepth_ != 0)
                fail("object construction requires owner-confined effect code", node->offset);
            const auto* object = declaration(node->type.name);
            if (!object || !objectClass(*object) || !node->type.arguments.empty())
                fail("new constructor syntax requires an ordinary non-exported Class", node->offset);
            auto* constructor = overload(declaration(object->name)->methods, object->name, node);
            if (!constructor->constructor || (!constructor->visible && owner_->name != object->name))
                fail("object constructor is private", node->offset);
            node->resolution = "object_construct";
            node->owner = object->name;
            node->callable = constructor->identity;
            singleTarget(node, constructor->identity);
            trackCalls(node->targets);
            result = nominal(*object);
        } else if (node->kind == "named_construct") {
            const auto* record = declaration(node->text);
            if (!record || record->kind != "Record")
                fail("named construction requires a record type", node->offset);
            std::set<std::string> assigned;
            for (const auto& child : node->children) {
                const auto* target = field(*record, child->text);
                if (!target || !assigned.insert(child->text).second)
                    fail("unknown or duplicate record initializer", child->offset);
                sameExpression(child->children[0], target->type);
            }
            result = nominal(*record);
        } else if (node->kind == "call") {
            result = call(node, allowServiceCall);
        } else if (node->kind == "await") {
            if (lockDepth_ != 0)
                fail("lock bodies cannot suspend", node->offset);
            if (parallelDepth_ != 0)
                fail("parallel workers cannot await", node->offset);
            if (finallyDepth_ != 0)
                fail("finally cannot suspend", node->offset);
            if (!allowAwait || !method_->asynchronous)
                fail("await must be a complete local initializer or assignment in an async method", node->offset);
            result = expression(node->children[0], false, true);
            if (result.name != "Task")
                fail("await requires a host or local Task<Result<T,string>> call", node->offset);
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
            sameExpression(node->children[index + 1], parameters[index].type);
    }
    Type call(const Expr& node, bool allowServiceCall) {
        const auto& target = node->children[0];
        if (target->kind == "name") {
            if (target->text == "Error") {
                arguments(node, {{stringType, "message", true, node->offset}});
                node->resolution = "error_construct";
                return errorType;
            }
            const auto* record = declaration(target->text);
            if (record && record->kind == "Record") {
                arguments(node, record->fields);
                node->resolution = "construct";
                return nominal(*record);
            }
            if (checkingBaseArguments_)
                fail("base arguments cannot call instance methods", node->offset);
            const auto* found = overload(*owner_, target->text, node);
            if (!found || found->constructor)
                fail("unknown local method", target->offset);
            if (found->asynchronous && (!allowServiceCall || !method_->asynchronous))
                fail("local async calls must be directly awaited in an async method", target->offset);
            if (method_->pure && !found->pure)
                fail("pure methods cannot call mutating helpers", target->offset);
            if (objectClass(*owner_)) {
                if (++objectNodes_ > 65536)
                    fail("object receiver normalization node limit exceeded", target->offset);
                auto member = std::make_shared<Expression>();
                member->kind = "member";
                member->text = target->text;
                member->offset = target->offset;
                member->children = {thisExpression(target->offset)};
                node->children[0] = std::move(member);
                return objectCall(node, nominal(*owner_), allowServiceCall, found);
            }
            node->resolution = found->asynchronous ? "local_async" : (found->pure ? "pure" : "effect");
            node->callable = found->identity;
            node->owner = owner_->name;
            singleTarget(node, found->identity);
            trackCalls(node->targets);
            return found->result;
        }
        if (target->kind != "member")
            fail("unsupported call target", node->offset);
        const auto& receiver = target->children[0];
        if (receiver->kind == "type" && isSynchronizationHandle(receiver->type.name)) {
            type(receiver->type, receiver->offset);
            if (method_->pure)
                fail("pure methods cannot construct synchronization state", node->offset);
            if (target->text != "Create")
                fail("unknown synchronization constructor", target->offset);
            if (receiver->type.name == "Lock") {
                arguments(node, {});
                node->resolution = "sync_lock_construct";
            } else {
                if (node->children.size() != 2 && node->children.size() != 3)
                    fail("SemaphoreSlim.Create requires initial count and optional maximum count", node->offset);
                std::vector<Field> parameters(node->children.size() - 1,
                    Field{integerType, "count", true, node->offset});
                arguments(node, parameters);
                node->resolution = "sync_construct";
            }
            return receiver->type;
        }
        if (receiver->kind == "type" && isConcurrentCollection(receiver->type.name)) {
            type(receiver->type, receiver->offset);
            if (method_->pure)
                fail("pure methods cannot create concurrent collection state", node->offset);
            if (target->text != "Empty")
                fail("unknown concurrent collection constructor", target->offset);
            arguments(node, {});
            node->resolution = "concurrent_construct";
            return receiver->type;
        }
        if (receiver->kind == "type" && receiver->type.name == "Atomic") {
            type(receiver->type, receiver->offset);
            if (method_->pure)
                fail("pure methods cannot create or observe atomic state", node->offset);
            if (target->text != "Create")
                fail("unknown Atomic constructor", target->offset);
            arguments(node, {{receiver->type.arguments[0], "value", true, node->offset}});
            node->resolution = "atomic_construct";
            return receiver->type;
        }
        if (receiver->kind == "type" && extended(receiver->type.name)) {
            type(receiver->type, receiver->offset);
            std::vector<Field> parameters;
            if (receiver->type.name == "Array" && target->text == "Create") {
                parameters = {{integerType, "size", true, node->offset},
                              {receiver->type.arguments[0], "initial", true, node->offset}};
            } else if (receiver->type.name == "KeyValuePair" && target->text == "Create") {
                parameters = {{receiver->type.arguments[0], "key", true, node->offset},
                              {receiver->type.arguments[1], "value", true, node->offset}};
            } else if (receiver->type.name != "Array" && receiver->type.name != "KeyValuePair" && target->text == "Empty") {
                parameters = {};
            } else {
                fail("unknown collection constructor", target->offset);
            }
            arguments(node, parameters);
            node->resolution = receiver->type.name == "KeyValuePair" ? "pair_construct" : "collection_construct";
            return receiver->type;
        }
        if (receiver->kind == "type" && receiver->type.name == "List") {
            type(receiver->type, receiver->offset);
            if (target->text != "Empty")
                fail("unknown List constructor", target->offset);
            arguments(node, {});
            node->resolution = "list_construct";
            return receiver->type;
        }
        if (receiver->kind == "type" && receiver->type == floatType) {
            if (target->text != "IsFinite" && target->text != "Abs" &&
                target->text != "Sqrt" && target->text != "Log")
                fail("unknown float operation", target->offset);
            arguments(node, {{floatType, "value", true, node->offset}});
            node->resolution = "float_math";
            return target->text == "IsFinite" ? booleanType : floatType;
        }
        if (receiver->kind == "type" && receiver->type.name == "Optional") {
            type(receiver->type, receiver->offset);
            std::vector<Field> parameters;
            if (target->text == "Some")
                parameters.push_back({receiver->type.arguments[0], "value", true, node->offset});
            else if (target->text != "None")
                fail("unknown Optional constructor", target->offset);
            arguments(node, parameters);
            node->resolution = "optional_construct";
            return receiver->type;
        }
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
        if (objectReference(receiverType))
            return objectCall(node, receiverType, allowServiceCall);
        if (receiverType.name == "SemaphoreSlim")
            return semaphoreCall(node, allowServiceCall);
        if (isConcurrentCollection(receiverType.name))
            return concurrentCall(node, receiverType, target->text);
        if (receiverType.name == "Atomic")
            return atomicCall(node, receiverType, target->text);
        if (extended(receiverType.name))
            return collectionCall(node, receiverType, target->text, receiver);
        if (receiverType.name == "List") {
            if (target->text == "At") {
                arguments(node, {{integerType, "index", true, node->offset}});
                node->resolution = "list_at";
                return receiverType.arguments[0];
            }
            if (target->text == "Add") {
                assignmentTarget(receiver);
                arguments(node, {{receiverType.arguments[0], "value", true, node->offset}});
                node->resolution = "list_add";
                return voidType;
            }
            fail("unknown List method", target->offset);
        }
        const auto* host = declaration(receiverType.name);
        if (!host || host->kind != "Interface" || !allowServiceCall || method_->pure)
            fail("host calls must be directly awaited in an async method", node->offset);
        auto* mutableHost = declaration(receiverType.name);
        const auto* found = overload(mutableHost->methods, target->text, node);
        if (!found->visible)
            fail("host interface method is private", target->offset);
        node->resolution = "host";
        return found->result;
    }
    static bool discardable(const Expr& node) {
        return node->resolution == "sync_release" || node->resolution == "atomic_store" || node->resolution == "atomic_exchange" ||
            node->resolution == "atomic_compareExchange" || node->resolution == "atomic_add" ||
            node->resolution == "atomic_increment" || node->resolution == "atomic_decrement" ||
            node->resolution.starts_with("collection_mutate_") ||
            node->resolution.starts_with("concurrent_mutate_");
    }
    Type semaphoreCall(const Expr& node, bool allowServiceCall) {
        if (method_->pure)
            fail("pure methods cannot observe or mutate semaphore state", node->offset);
        const auto& operation = node->children[0]->text;
        if (operation == "Release") {
            if (node->children.size() > 2)
                fail("SemaphoreSlim.Release accepts zero or one count", node->offset);
            if (node->children.size() == 2)
                same(expression(node->children[1]), integerType, node->children[1]->offset);
            node->resolution = "sync_release";
            return integerType;
        }
        const bool asynchronous = operation == "WaitAsync";
        if (operation != "Wait" && !asynchronous)
            fail("unknown SemaphoreSlim operation", node->offset);
        if (asynchronous) {
            if (!allowServiceCall || !method_->asynchronous)
                fail("SemaphoreSlim.WaitAsync must be directly awaited in an async method", node->offset);
            if (lockDepth_ != 0 || finallyDepth_ != 0 || parallelDepth_ != 0)
                fail("SemaphoreSlim.WaitAsync cannot suspend in lock, finally or parallel work", node->offset);
        } else if (method_->asynchronous || lockDepth_ != 0 || finallyDepth_ != 0) {
            fail("blocking SemaphoreSlim.Wait is forbidden in async methods, lock bodies and finally", node->offset);
        }
        std::vector<Type> actual;
        for (std::size_t index = 1; index < node->children.size(); ++index)
            actual.push_back(expression(node->children[index]));
        bool timed = false;
        if (actual.empty() || (actual.size() == 1 && actual[0] == cancellationType)) {
            timed = false;
        } else if ((actual.size() == 1 && actual[0] == integerType) ||
                   (actual.size() == 2 && actual[0] == integerType && actual[1] == cancellationType)) {
            timed = true;
        } else {
            fail("semaphore waits accept an optional timeout followed by an optional cancellation token", node->offset);
        }
        node->resolution = asynchronous ? "sync_wait_async" : "sync_wait";
        if (timed)
            node->resolution += "_timed";
        const auto result = timed ? booleanType : voidType;
        return asynchronous ? Type{"Task", {result}} : result;
    }
    Type concurrentCall(const Expr& node, const Type& receiver, const std::string& name) {
        if (method_->pure)
            fail("pure methods cannot observe or mutate concurrent collection state", node->offset);
        const bool dictionary = receiver.name == "ConcurrentDictionary";
        const auto& element = receiver.arguments[0];
        const auto value = dictionary ? receiver.arguments[1] : element;
        std::vector<Type> parameters;
        Type result = voidType;
        std::string operation;
        bool mutating = false;
        if (dictionary && (name == "TryAdd" || name == "Set" || name == "GetOrAdd")) {
            parameters = {element, value};
            if (name == "TryAdd") {
                result = booleanType;
                operation = "tryAdd";
            } else if (name == "GetOrAdd") {
                result = value;
                operation = "getOrAdd";
            } else {
                operation = "set";
            }
            mutating = true;
        } else if (dictionary && name == "TryUpdate") {
            parameters = {element, value, value};
            result = booleanType;
            operation = "tryUpdate";
            mutating = true;
        } else if (dictionary && (name == "At" || name == "TryGet" ||
                                 name == "ContainsKey" || name == "TryRemove")) {
            parameters = {element};
            if (name == "At") {
                result = value;
                operation = "at";
            } else if (name == "ContainsKey") {
                result = booleanType;
                operation = "containsKey";
            } else {
                result = {"Optional", {value}};
                operation = name == "TryGet" ? "tryGet" : "tryRemove";
                mutating = name == "TryRemove";
            }
        } else if ((receiver.name == "ConcurrentQueue" && name == "Enqueue") ||
                   (receiver.name == "ConcurrentStack" && name == "Push")) {
            parameters = {element};
            operation = name == "Enqueue" ? "enqueue" : "push";
            mutating = true;
        } else if ((receiver.name == "ConcurrentQueue" && name == "TryDequeue") ||
                   (receiver.name == "ConcurrentStack" && name == "TryPop") ||
                   (!dictionary && name == "TryPeek")) {
            result = {"Optional", {element}};
            if (name == "TryPeek") {
                operation = "tryPeek";
            } else {
                operation = name == "TryDequeue" ? "tryDequeue" : "tryPop";
                mutating = true;
            }
        } else if (!dictionary && name == "ToList") {
            result = {"List", {element}};
            operation = "items";
        } else if (name == "Clear") {
            operation = "clear";
            mutating = true;
        } else {
            fail("unknown concurrent collection operation", node->offset);
        }
        std::vector<Field> expected;
        for (const auto& parameter : parameters)
            expected.push_back({parameter, "argument", true, node->offset});
        arguments(node, expected);
        node->resolution = std::string(mutating ? "concurrent_mutate_" : "concurrent_read_") + operation;
        return result;
    }
    Type atomicCall(const Expr& node, const Type& receiver, const std::string& name) {
        if (method_->pure)
            fail("pure methods cannot create or observe atomic state", node->offset);
        std::vector<Field> parameters;
        const auto& value = receiver.arguments[0];
        Type result = value;
        if (name == "Load") {
            node->resolution = "atomic_load";
        } else if (name == "Store" || name == "Exchange" || name == "Add") {
            if (name == "Add")
                same(value, integerType, node->offset);
            parameters.push_back({value, "value", true, node->offset});
            node->resolution = name == "Store" ? "atomic_store" : (name == "Exchange" ? "atomic_exchange" : "atomic_add");
            if (name == "Store")
                result = voidType;
        } else if (name == "CompareExchange") {
            parameters = {{value, "value", true, node->offset}, {value, "comparand", true, node->offset}};
            node->resolution = "atomic_compareExchange";
        } else if (name == "Increment" || name == "Decrement") {
            same(value, integerType, node->offset);
            node->resolution = name == "Increment" ? "atomic_increment" : "atomic_decrement";
        } else {
            fail("unknown Atomic operation", node->offset);
        }
        arguments(node, parameters);
        return result;
    }
    Type collectionCall(const Expr& node, const Type& receiver, const std::string& name, const Expr& target) {
        const auto& element = receiver.arguments[0];
        const bool dictionary = receiver.name == "Dictionary";
        const auto value = dictionary ? receiver.arguments[1] : element;
        std::vector<Type> parameters;
        Type result = voidType;
        std::string operation;
        bool mutating = false;
        if ((receiver.name == "Array" || dictionary) && name == "At") {
            parameters = {dictionary ? element : integerType};
            result = value;
            operation = "at";
        } else if ((receiver.name == "Array" || dictionary) && name == "Set") {
            parameters = {dictionary ? element : integerType, value};
            operation = "set";
            mutating = true;
        } else if (dictionary && name == "Add") {
            parameters = {element, value};
            operation = "add";
            mutating = true;
        } else if (dictionary && (name == "TryGet" || name == "ContainsKey" || name == "Remove")) {
            parameters = {element};
            result = name == "TryGet" ? Type{"Optional", {value}} : booleanType;
            operation = name == "TryGet" ? "tryGet" : (name == "ContainsKey" ? "containsKey" : "remove");
            mutating = name == "Remove";
        } else if (receiver.name == "HashSet" && (name == "Add" || name == "Remove" || name == "Contains")) {
            parameters = {element};
            result = booleanType;
            operation = name == "Add" ? "add" : (name == "Remove" ? "remove" : "contains");
            mutating = name != "Contains";
        } else if (receiver.name == "HashSet" && name == "ToList") {
            result = {"List", {element}};
            operation = "items";
        } else if ((receiver.name == "Queue" && name == "Enqueue") || (receiver.name == "Stack" && name == "Push")) {
            parameters = {element};
            operation = name == "Enqueue" ? "enqueue" : "push";
            mutating = true;
        } else if ((receiver.name == "Queue" && (name == "Dequeue" || name == "TryDequeue")) ||
                   (receiver.name == "Stack" && (name == "Pop" || name == "TryPop"))) {
            const bool optional = name == "TryDequeue" || name == "TryPop";
            result = optional ? Type{"Optional", {element}} : element;
            operation = name == "Dequeue" ? "dequeue" : (name == "TryDequeue" ? "tryDequeue" :
                (name == "Pop" ? "pop" : "tryPop"));
            mutating = true;
        } else if ((receiver.name == "Queue" || receiver.name == "Stack") && name == "Peek") {
            result = element;
            operation = "peek";
        } else if (receiver.name != "Array" && receiver.name != "KeyValuePair" && name == "Clear") {
            operation = "clear";
            mutating = true;
        } else {
            fail("unknown collection operation", node->offset);
        }
        if (mutating)
            assignmentTarget(target);
        std::vector<Field> expected;
        for (std::size_t index = 0; index < parameters.size(); ++index)
            expected.push_back({parameters[index], "argument", true, node->offset});
        arguments(node, expected);
        node->resolution = std::string(mutating ? "collection_mutate_" : "collection_read_") + operation;
        return result;
    }
    void checkWorkerCalls(Declaration& owner) {
        std::set<std::string> checked;
        std::function<void(Method&)> worker;
        std::function<void(const Expr&)> inspectExpression;
        std::function<void(const std::vector<Statement>&)> inspectStatements;
        inspectExpression = [&](const Expr& node) {
            if (!node)
                return;
            if (referenceBearing(node->type) || node->resolution == "this" || node->resolution == "object_field")
                fail("parallel helper cannot access owner-confined objects", node->offset);
            if (node->resolution == "service" || node->resolution == "host" || node->resolution == "local_async" || node->kind == "await")
                fail("parallel helper cannot access host services or suspend", node->offset);
            if (node->kind == "call" && (node->resolution == "pure" || node->resolution == "effect")) {
                if (node->targets.empty())
                    worker(*method(node->callable));
                else {
                    for (const auto& identity : node->targets)
                        worker(*method(identity));
                }
            }
            if ((node->resolution == "list_add" || node->resolution.starts_with("collection_mutate_")) && node->kind == "call") {
                auto root = node->children[0]->children[0];
                while (root->kind == "member")
                    root = root->children[0];
                if (root->resolution == "field")
                    fail("parallel helper cannot mutate ordinary model state", root->offset);
            }
            for (const auto& child : node->children)
                inspectExpression(child);
        };
        inspectStatements = [&](const std::vector<Statement>& body) {
            for (const auto& statement : body) {
                if (statement.kind == "parallel_foreach")
                    fail("nested parallel work through a helper is not admitted", statement.offset);
                if (statement.kind == "assign") {
                    auto root = statement.target;
                    while (root->kind == "member")
                        root = root->children[0];
                    if (root->resolution == "field" || root->resolution == "service")
                        fail("parallel helper cannot mutate ordinary model state", root->offset);
                }
                inspectExpression(statement.expression);
                inspectExpression(statement.target);
                inspectStatements(statement.body);
                inspectStatements(statement.alternative);
                inspectStatements(statement.cleanup);
            }
        };
        worker = [&](Method& target) {
            if (!checked.insert(target.identity).second)
                return;
            OwnerContext scope(owner_, methodOwner(target));
            if (target.asynchronous || target.constructor)
                fail("parallel helper must be synchronous", target.offset);
            target.workerCallable = true;
            inspectStatements(target.body);
        };
        for (const auto& target : owner.methods) {
            for (const auto& identity : target.parallelCalls)
                worker(*method(identity));
        }
        std::set<std::string> cleanupChecked;
        std::function<void(const Method&)> cleanup = [&](const Method& target) {
            if (!cleanupChecked.insert(target.identity).second)
                return;
            OwnerContext scope(owner_, methodOwner(target));
            std::function<void(const std::vector<Statement>&)> inspect = [&](const std::vector<Statement>& body) {
                for (const auto& statement : body) {
                    if (statement.kind == "parallel_foreach")
                        fail("finally cannot start parallel work through a helper", statement.offset);
                    inspect(statement.body);
                    inspect(statement.alternative);
                    inspect(statement.cleanup);
                }
            };
            inspect(target.body);
            for (const auto& identity : target.calls)
                cleanup(*method(identity));
        };
        for (const auto& target : owner.methods) {
            for (const auto& identity : target.cleanupCalls)
                cleanup(*method(identity));
        }
    }
    void checkSynchronizationCalls(const Declaration& owner) {
        enum class Context { Async, Lock, Cleanup };
        auto checkClosure = [&](const Method& root, Context context) {
            std::set<std::string> checked;
            std::function<void(const Method&)> visit;
            std::function<void(const Expr&)> inspectExpression;
            std::function<void(const std::vector<Statement>&)> inspectStatements;
            inspectExpression = [&](const Expr& node) {
                if (!node)
                    return;
                if (node->resolution == "sync_wait" || node->resolution == "sync_wait_timed")
                    fail("blocking SemaphoreSlim.Wait is forbidden in async, lock and finally call closures", node->offset);
                if (context == Context::Lock &&
                    (node->kind == "await" || node->resolution == "local_async" ||
                     node->resolution == "sync_wait_async" || node->resolution == "sync_wait_async_timed"))
                    fail("lock call closures cannot suspend", node->offset);
                for (const auto& child : node->children)
                    inspectExpression(child);
            };
            inspectStatements = [&](const std::vector<Statement>& statements) {
                for (const auto& statement : statements) {
                    if (context == Context::Lock && statement.kind == "parallel_foreach")
                        fail("lock call closures cannot start parallel work", statement.offset);
                    inspectExpression(statement.expression);
                    inspectExpression(statement.target);
                    inspectStatements(statement.body);
                    inspectStatements(statement.alternative);
                    inspectStatements(statement.cleanup);
                }
            };
            visit = [&](const Method& target) {
                if (!checked.insert(target.identity).second)
                    return;
                OwnerContext scope(owner_, methodOwner(target));
                if (context == Context::Lock && target.asynchronous)
                    fail("lock call closures cannot call async methods", target.offset);
                inspectStatements(target.body);
                inspectExpression(target.baseConstructor);
                for (const auto& initializer : target.fieldInitializers)
                    inspectExpression(initializer);
                for (const auto& identity : target.calls)
                    visit(*method(identity));
            };
            visit(root);
        };
        for (const auto& target : owner.methods) {
            if (target.asynchronous)
                checkClosure(target, Context::Async);
            for (const auto& identity : target.lockCalls)
                checkClosure(*method(identity), Context::Lock);
            for (const auto& identity : target.cleanupCalls)
                checkClosure(*method(identity), Context::Cleanup);
        }
    }
    void assignmentTarget(const Expr& target) {
        expression(target);
        Expr root = target;
        while (root->kind == "member") {
            if (root->resolution == "object_field")
                break;
            if (root->resolution != "record")
                fail("assignment requires mutable record field", root->offset);
            root = root->children[0];
        }
        if (root->resolution == "object_field") {
            if (method_->pure || parallelDepth_ != 0)
                fail("owner-confined object fields require an owner effect", root->offset);
            const bool ownConstruction = method_->constructor && owner_->name == root->owner &&
                (root->kind == "name" || (root->kind == "member" && root->children[0]->kind == "this"));
            if (isSharedHandle(root->type) && !ownConstruction)
                fail("object shared field identity is fixed by its constructor", root->offset);
            if (service(root->type) && !ownConstruction)
                fail("object host service identity is fixed by its constructor", root->offset);
            return;
        }
        if (root->kind != "name" || (root->resolution != "field" && root->resolution != "local" && root->resolution != "service"))
            fail("invalid assignment target", root->offset);
        if (root->resolution == "local" &&
            std::find(caughtErrors_.begin(), caughtErrors_.end(), root->text) != caughtErrors_.end())
            fail("catch Error bindings are read-only", root->offset);
        if (parallelDepth_ != 0 && (root->resolution != "local" || parallelOuter_.contains(root->text)))
            fail("parallel workers cannot mutate ordinary outer captures or model state", root->offset);
        if (root->resolution == "field" && isConcurrentCollection(root->type.name) && !method_->constructor)
            fail("concurrent collection field identity is fixed by the constructor", root->offset);
        if (root->resolution == "field" && isSynchronizationHandle(root->type.name) && !method_->constructor)
            fail("synchronization field identity is fixed by the constructor", root->offset);
        if (root->resolution == "field" && root->type.name == "Atomic" && !method_->constructor)
            fail("atomic field identity is fixed by the constructor", root->offset);
        if (method_->pure && root->resolution != "local")
            fail("pure methods cannot mutate model fields", root->offset);
        if (root->resolution == "service" && !method_->constructor)
            fail("service identity is fixed by the constructor", root->offset);
    }
    enum Flow : unsigned {
        Fallthrough = 1,
        Returned = 2,
        Thrown = 4,
        Broken = 8,
        Continued = 16
    };
    unsigned statements(std::vector<Statement>& body) {
        unsigned flow = Fallthrough;
        for (auto& statement : body) {
            if ((flow & Fallthrough) == 0)
                fail("unreachable statement", statement.offset);
            unsigned next = Fallthrough;
            if (statement.kind == "local") {
                type(statement.type, statement.offset);
                if ((!sourceValue(statement.type) && !isSharedHandle(statement.type)) ||
                    locals_.contains(statement.name) || field(*owner_, statement.name))
                    fail("local must be an owned value with a unique unshadowed name", statement.offset);
                sameExpression(statement.expression, statement.type, true);
                locals_[statement.name] = statement.type;
            } else if (statement.kind == "expression") {
                const bool awaited = statement.expression->kind == "await";
                if (statement.expression->kind != "call" && !awaited)
                    fail("expression statements require a method call", statement.offset);
                const auto result = expression(statement.expression, awaited);
                if (awaited && (result != voidType || statement.expression->children[0]->kind != "call" ||
                    statement.expression->children[0]->resolution != "sync_wait_async"))
                    fail("await expression statements require an infinite SemaphoreSlim.WaitAsync overload", statement.offset);
                if (result != voidType && !discardable(statement.expression))
                    fail("non-void call result must be consumed", statement.offset);
            } else if (statement.kind == "assign") {
                assignmentTarget(statement.target);
                sameExpression(statement.expression, statement.target->type, true);
            } else if (statement.kind == "return") {
                if (parallelDepth_ != 0)
                    fail("parallel iteration cannot return from its enclosing method", statement.offset);
                if (finallyDepth_ != 0)
                    fail("finally cannot return", statement.offset);
                if (method_->constructor)
                    fail("constructor returns are not supported; initialize all services and fields", statement.offset);
                const Type expected = method_->asynchronous ? method_->result.arguments[0] : method_->result;
                const auto actual = statement.expression ? expression(statement.expression) : voidType;
                same(actual, expected, statement.offset);
                if (statement.expression && actual != expected)
                    statement.expression->convertTo = expected;
                next = Returned;
            } else if (statement.kind == "throw") {
                if (statement.expression) {
                    same(expression(statement.expression), errorType, statement.offset);
                } else {
                    if (caughtErrors_.empty())
                        fail("rethrow requires a lexically enclosing catch", statement.offset);
                    statement.name = caughtErrors_.back();
                }
                next = Thrown;
            } else if (statement.kind == "break" || statement.kind == "continue") {
                if (loopDepth_ == 0)
                    fail("break and continue require a loop in the same cleanup scope", statement.offset);
                next = statement.kind == "break" ? Broken : Continued;
            } else if (statement.kind == "parallel_foreach") {
                if (method_->pure || parallelDepth_ != 0 || finallyDepth_ != 0 || lockDepth_ != 0)
                    fail("parallel foreach requires an effect method outside workers, cleanup and lock bodies", statement.offset);
                type(statement.type, statement.offset);
                same(sequenceItem(expression(statement.expression), statement.offset), statement.type, statement.offset);
                if (referenceBearing(statement.type) || referenceBearing(statement.expression->type))
                    fail("parallel inputs cannot contain owner-confined object references", statement.offset);
                if (locals_.contains(statement.name) || field(*owner_, statement.name))
                    fail("parallel item requires a unique unshadowed local", statement.offset);
                auto outer = locals_;
                parallelUsed_.clear();
                parallelOuter_ = outer;
                locals_[statement.name] = statement.type;
                const auto outerLoopDepth = loopDepth_;
                loopDepth_ = 0;
                ++parallelDepth_;
                statements(statement.body);
                --parallelDepth_;
                loopDepth_ = outerLoopDepth;
                for (const auto& name : parallelUsed_)
                    statement.parallelCaptures.push_back({outer.at(name), name, true, statement.offset});
                parallelUsed_.clear();
                parallelOuter_.clear();
                locals_ = std::move(outer);
                next = Fallthrough | Thrown;
            } else if (statement.kind == "while" || statement.kind == "foreach") {
                auto outer = locals_;
                if (statement.kind == "while") {
                    same(expression(statement.expression), booleanType, statement.offset);
                } else {
                    type(statement.type, statement.offset);
                    const auto itemType = sequenceItem(expression(statement.expression), statement.offset);
                    same(itemType, statement.type, statement.offset);
                    if (itemType != statement.type)
                        statement.itemConvertTo = statement.type;
                    if (!sourceValue(statement.type) || locals_.contains(statement.name) || field(*owner_, statement.name))
                        fail("foreach requires a unique unshadowed owned local", statement.offset);
                    locals_[statement.name] = statement.type;
                }
                ++loopDepth_;
                const auto loopFlow = statements(statement.body);
                --loopDepth_;
                locals_ = std::move(outer);
                // A loop may execute zero times. Local break/continue are
                // consumed here rather than proving an enclosing return.
                next = Fallthrough | (loopFlow & (Returned | Thrown));
            } else if (statement.kind == "lock") {
                if (method_->pure)
                    fail("pure methods cannot acquire locks", statement.offset);
                same(expression(statement.expression), Type{"Lock", {}}, statement.offset);
                auto outer = locals_;
                ++lockDepth_;
                next = statements(statement.body);
                --lockDepth_;
                locals_ = std::move(outer);
            } else if (statement.kind == "block") {
                auto outer = locals_;
                next = statements(statement.body);
                locals_ = std::move(outer);
            } else if (statement.kind == "if") {
                same(expression(statement.expression), booleanType, statement.offset);
                auto outer = locals_;
                const auto first = statements(statement.body);
                locals_ = outer;
                const auto second = statements(statement.alternative);
                locals_ = std::move(outer);
                next = first | second;
            } else if (statement.kind == "try") {
                auto outer = locals_;
                next = statements(statement.body);
                statement.bodyFallsThrough = (next & Fallthrough) != 0;
                locals_ = outer;
                if (!statement.name.empty()) {
                    same(statement.type, errorType, statement.offset);
                    if (locals_.contains(statement.name) || field(*owner_, statement.name))
                        fail("catch requires a unique unshadowed Error binding", statement.offset);
                    locals_[statement.name] = errorType;
                    caughtErrors_.push_back(statement.name);
                    const auto recovery = statements(statement.alternative);
                    caughtErrors_.pop_back();
                    // Every protected expression may fault. A catch path is
                    // relevant even without an explicit source throw.
                    next = (next & ~Thrown) | recovery;
                    locals_ = outer;
                }
                if (statement.hasFinally) {
                    const auto outerLoopDepth = loopDepth_;
                    loopDepth_ = 0;
                    ++finallyDepth_;
                    const auto cleanup = statements(statement.cleanup);
                    --finallyDepth_;
                    loopDepth_ = outerLoopDepth;
                    locals_ = outer;
                    if ((cleanup & Fallthrough) == 0)
                        next = Thrown;
                    else
                        next |= cleanup & Thrown;
                }
                locals_ = std::move(outer);
            } else {
                fail("unsupported statement", statement.offset);
            }
            flow = (flow & ~Fallthrough) | next;
        }
        return flow;
    }
    void checkConstantInitializer(const Expr& node) {
        if (node->resolution == "field" || node->resolution == "service" ||
            node->resolution == "local" || node->resolution == "pure" || node->resolution == "effect")
            fail("record defaults must be independent value expressions", node->offset);
        for (const auto& child : node->children)
            checkConstantInitializer(child);
    }
    void checkRecordCycles() {
        struct OwnerScope {
            Declaration*& target;
            Declaration* previous;
            OwnerScope(Declaration*& current, Declaration* replacement)
                : target(current), previous(current) { target = replacement; }
            ~OwnerScope() { target = previous; }
        };
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
            OwnerScope scope(owner_, types_.at(record.name));
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
        for (auto& declaration : program_.declarations) {
            OwnerScope scope(owner_, &declaration);
            if (declaration.kind == "Record") {
                visitRecord(declaration);
            } else if (declaration.kind == "Class") {
                Layout layout;
                for (const auto& field : declaration.fields) {
                    if (!service(field.type) && !isSharedHandle(field.type))
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
            const bool localCall = node->resolution == "pure" || node->resolution == "effect" || node->resolution == "local_async";
            const bool objectCall = node->resolution == "object_construct" || node->resolution == "object_call" ||
                node->resolution == "object_async" || node->resolution == "source_interface_call" ||
                node->resolution == "source_interface_async" || node->resolution == "class_virtual_call" ||
                node->resolution == "class_virtual_async";
            if (localCall || objectCall) {
                std::size_t maximum = 0;
                if (node->targets.empty())
                    maximum = visit(*this->method(node->callable)).work;
                else {
                    for (const auto& identity : node->targets)
                        maximum = std::max(maximum, visit(*this->method(identity)).work);
                }
                work = addCost(work, maximum, node->offset);
            }
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
                work = addCost(work, statementCost(statement.cleanup), statement.offset);
            }
            return work;
        };
        visit = [&](const Method& method) -> Cost {
            OwnerContext scope(owner_, methodOwner(method));
            if (visiting.contains(method.identity))
                fail("recursive local call graph", method.offset);
            if (auto found = costs.find(method.identity); found != costs.end())
                return found->second;
            if (program_.objectEnabled && visiting.size() >= 32)
                fail("local call depth limit exceeded", method.offset);
            visiting.insert(method.identity);
            Cost cost;
            cost.depth = 1;
            for (const auto& name : method.calls)
                cost.depth = std::max(cost.depth, visit(*this->method(name)).depth + 1);
            if (cost.depth > 32)
                fail("local call depth limit exceeded", method.offset);
            cost.work = statementCost(method.body);
            if (method.constructor) {
                cost.work = addCost(cost.work, expressionCost(method.baseConstructor), method.offset);
                for (const auto& initializer : method.fieldInitializers)
                    cost.work = addCost(cost.work, expressionCost(initializer), method.offset);
            }
            visiting.erase(method.identity);
            costs[method.identity] = cost;
            return cost;
        };
        for (const auto& method : owner.methods)
            visit(method);
    }
    bool checkingBaseArguments_ = false;
    std::size_t initializerNodes_ = 0;
    std::size_t objectNodes_ = 0;
    std::size_t callEdges_ = 0;
    std::size_t callIdentityBytes_ = 0;
    Program& program_;
    std::map<std::string, Declaration*> types_;
    std::map<std::string, Method*> methods_;
    std::map<std::string, std::vector<std::string>> dispatch_;
    Declaration* owner_ = nullptr;
    Method* method_ = nullptr;
    std::map<std::string, Type> locals_;
    unsigned loopDepth_ = 0;
    unsigned finallyDepth_ = 0;
    unsigned parallelDepth_ = 0;
    unsigned lockDepth_ = 0;
    std::map<std::string, Type> parallelOuter_;
    std::set<std::string> parallelUsed_;
    std::vector<std::string> caughtErrors_;
};

}

void check(Program& program) {
    Checker(program).run();
}

}
