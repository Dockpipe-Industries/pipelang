// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <pipelang/model/collection.hpp>
#include <pipelang/model/floating_environment.hpp>

namespace pipelang::models {
namespace {

const Type booleanType{"bool", {}};
const Type integerType{"int", {}};
const Type unsignedType{"uint32", {}};
const Type floatType{"float", {}};
const Type float32Type{"float32", {}};
const Type stringType{"string", {}};
const Type byteBufferType{"ByteBuffer", {}};
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
                declaration.name == "FixedArray" ||
                declaration.name == "Atomic" || extended(declaration.name) || isConcurrentCollection(declaration.name) ||
                isSynchronizationHandle(declaration.name))
                fail("duplicate or reserved type '" + declaration.name + "'", declaration.offset);
            types_[declaration.name] = &declaration;
        }
        const bool exportsConstants = std::any_of(program_.declarations.begin(), program_.declarations.end(), [](const auto& owner) {
            return std::any_of(owner.constants.begin(), owner.constants.end(), [](const auto& value) { return value.visible; });
        });
        if (exportsConstants && types_.contains("constants")) {
            owner_ = types_.at("constants");
            fail("constant export namespace conflicts with source type 'constants'", owner_->offset);
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
        for (auto& declaration : program_.declarations) {
            for (auto& constant : declaration.constants)
                ensureConstant(constant, declaration);
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
            for (auto& declaration : program_.declarations) {
                for (auto& method : declaration.methods) {
                    method.dispatchTargets.clear();
                    const auto found = dispatch_.find(method.identity);
                    if (found != dispatch_.end())
                        method.dispatchTargets = found->second;
                }
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
        if (owner_) {
            diagnostic.module = owner_->module;
            diagnostic.path = owner_->originPath;
        }
        throw diagnostic;
    }
    static bool extended(const std::string& name) {
        return name == "Array" || name == "FixedArray" || name == "Dictionary" || name == "HashSet" ||
            name == "Queue" || name == "Stack" || name == "KeyValuePair";
    }
    bool keyType(const Type& value) const {
        const auto* found = declaration(value.name);
        return value.arguments.empty() && (value.name == "int" || value.name == "uint32" || value.name == "bool" ||
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
    bool pureHostValue(const Type& value, std::set<std::string>& records,
                       std::set<std::string>& completed) const {
        if (!sourceValue(value) || referenceBearing(value))
            return false;
        for (std::size_t index = 0; index < value.arguments.size(); ++index) {
            const auto& argument = value.arguments[index];
            if (value.name == "Result" && index == 0 && argument.name == "void")
                continue;
            if (!pureHostValue(argument, records, completed))
                return false;
        }
        const auto* record = declaration(value.name);
        if (!record || record->kind != "Record")
            return true;
        if (completed.contains(record->name))
            return true;
        if (!records.insert(record->name).second)
            return false;
        for (const auto& member : record->fields) {
            if (!pureHostValue(member.type, records, completed))
                return false;
        }
        records.erase(record->name);
        completed.insert(record->name);
        return true;
    }
    bool pureHostValue(const Type& value) const {
        std::set<std::string> records;
        std::set<std::string> completed;
        return pureHostValue(value, records, completed);
    }
    bool assignable(const Type& actual, const Type& expected) const {
        if (actual == expected)
            return true;
        if (expected.name == "Optional" && expected.arguments.size() == 1) {
            if (actual.name == "$null")
                return true;
            return assignable(actual, expected.arguments[0]);
        }
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
        if (sequence.name == "List" || sequence.name == "Array" || sequence.name == "FixedArray" || sequence.name == "HashSet" ||
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
        return name == "void" || name == "bool" || name == "int" || name == "uint32" || name == "float" || name == "float32" || name == "string" || name == "ByteBuffer" || name == "Error";
    }
    Declaration* declaration(const std::string& name) const {
        auto found = types_.find(name);
        return found == types_.end() ? nullptr : found->second;
    }
    void type(const Type& value, std::size_t offset, bool allowVoid = false, bool allowTask = false) {
        if (value.fixedExtent && value.name != "FixedArray")
            fail("only FixedArray accepts a literal extent", offset);
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
            if (value.arguments.size() == 1 && value.arguments[0] == unsignedType)
                fail("Atomic<uint32> is not supported", offset);

            if (value.arguments.size() != 1 ||
                (value.arguments[0] != integerType && value.arguments[0] != booleanType && value.arguments[0] != floatType && value.arguments[0] != float32Type))
                fail("Atomic requires int, bool, float or float32", offset);
            return;
        }
        if (value.name == "FixedArray") {
            if (value.arguments.size() != 1 || !value.fixedExtent ||
                *value.fixedExtent > pipelang::model::maximumListElements)
                fail("FixedArray requires an owned value type and a literal extent between 0 and 4096", offset);
            type(value.arguments.front(), offset);
            if (!plainValue(value.arguments.front()))
                fail("FixedArray requires deeply owned value elements", offset);
            checkFixedArrayFootprint(value, offset);
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
                fail("nullable suffix requires one owned value type", offset);
            type(value.arguments[0], offset);
            if (!sourceValue(value.arguments[0]))
                fail("nullable suffix requires an owned value payload", offset);
            return;
        }
        if (value.name == "Result") {
            if (value.arguments.size() != 2)
                fail("Result requires value and error types", offset);
            type(value.arguments[0], offset, true);
            type(value.arguments[1], offset);
            if (value.arguments[0].name != "void" && !sourceValue(value.arguments[0]))
                fail("Result payloads must recursively contain owned values", offset);
            const auto* error = declaration(value.arguments[1].name);
            const bool declaredErrorValue = value.arguments[1].arguments.empty() && error &&
                (error->kind == "Enum" || error->kind == "Record") && sourceValue(value.arguments[1]);
            if (value.arguments[1] != stringType && !declaredErrorValue)
                fail("Result errors require string, a declared enum or an owned record", offset);
            return;
        }
        if (value.name == "Task") {
            if (!allowTask || value.arguments.size() != 1 || value.arguments[0].name != "Result")
                fail("Task is supported only as Task<Result<T,string>> method return", offset);
            type(value.arguments[0], offset);
            if (value.arguments[0].arguments[1] != stringType)
                fail("Task requires string Result errors", offset);
            return;
        }
        if (!value.arguments.empty())
            fail("unsupported generic type '" + value.displayText() + "'", offset);
        if (primitive(value.name) || value.name == "CancellationToken")
            return;
        const auto* found = declaration(value.name);
        if (!found || (found->kind == "Class" && !objectClass(*found)) ||
            (found->sourceInterface && !program_.objectEnabled))
            fail("unknown value or service type '" + value.displayText() + "'", offset);
    }
    void checkFixedArrayFootprint(const Type& value, std::size_t offset) {
        constexpr auto maximum = pipelang::model::maximumListElements;
        struct Footprint {
            std::size_t storage = 1;
            std::size_t elements = 0;
        };
        std::size_t remaining = pipelang::model::defaultEvaluationSteps;
        std::set<std::string> activeRecords;
        const auto add = [](std::size_t total, std::size_t added) {
            return total > maximum || added > maximum - total ? maximum + 1 : total + added;
        };
        std::function<Footprint(const Type&)> visit;
        visit = [&](const Type& current) -> Footprint {
            if (remaining == 0)
                fail("fixed value layout evaluation limit exceeded", offset);
            --remaining;
            if (current.name == "FixedArray") {
                if (current.arguments.size() != 1 || !current.fixedExtent || *current.fixedExtent > maximum)
                    fail("invalid FixedArray shape", offset);
                // Visit the element even for an empty array: zero storage does
                // not make recursive or otherwise invalid element types sound.
                const auto child = visit(current.arguments.front());
                const auto extent = *current.fixedExtent;
                if (extent == 0)
                    return Footprint{};
                const auto perElement = add(1, child.elements);
                if (child.storage > maximum / extent || perElement > maximum / extent)
                    fail("aggregate fixed value layout limit exceeded", offset);
                return {extent * child.storage, extent * perElement};
            }
            const auto* record = declaration(current.name);
            if (record && record->kind == "Record") {
                if (!activeRecords.insert(record->name).second)
                    fail("recursive record value graph is not supported", offset);
                std::size_t total = 0;
                std::size_t elements = 0;
                for (const auto& field : record->fields) {
                    const auto child = visit(field.type);
                    total = add(total, child.storage);
                    elements = add(elements, child.elements);
                }
                activeRecords.erase(record->name);
                return {std::max(std::size_t{1}, total), elements};
            }
            std::size_t total = 1;
            std::size_t elements = 0;
            for (const auto& argument : current.arguments) {
                if (argument.name != "void") {
                    const auto child = visit(argument);
                    total = add(total, child.storage);
                    elements = add(elements, child.elements);
                }
            }
            return {total, elements};
        };
        (void)visit(value);
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
    const Declaration& accessOwner(const Declaration& owner) const {
        if (!program_.projectEnabled || owner.sourceObjectFacet.empty())
            return owner;
        const auto* facet = declaration(owner.sourceObjectFacet);
        if (!facet || !facet->generatedObjectFacet)
            fail("project source facet identity was lost", owner.offset);
        return *facet;
    }
    bool accessible(bool visible, bool protectedMember, const Declaration& declaring) const {
        if (visible)
            return true;
        // Lowering creates distinct runtime receivers, not distinct source
        // classes. Private/protected access follows the authoritative class.
        const auto& current = accessOwner(*owner_);
        const auto& target = accessOwner(declaring);
        return current.name == target.name ||
            (protectedMember && derivesFrom(current, target.name));
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
                if (!method.constructor && !owner.baseClass.empty() && constant(*declaration(owner.baseClass), method.name))
                    fail("inherited constants cannot be hidden by methods", method.offset);
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
        for (auto& constant : owner.constants) {
            type(constant.type, constant.offset);
            if (constant.type != booleanType && constant.type != integerType && constant.type != unsignedType &&
                constant.type != floatType && constant.type != float32Type)
                fail("const requires bool, int, uint32, float or float32", constant.offset);
            if (!constant.initializer)
                fail("const requires an initializer", constant.offset);
            if (!members.insert(constant.name).second)
                fail("duplicate member '" + constant.name + "'", constant.offset);
            if (!owner.baseClass.empty() && field(*declaration(owner.baseClass), constant.name))
                fail("inherited values cannot be hidden by constants", constant.offset);
            constant.declaringClass = owner.name;
        }
        for (const auto& field : owner.fields) {
            if (!owner.baseClass.empty() && constant(*declaration(owner.baseClass), field.name))
                fail("inherited constants cannot be hidden by fields", field.offset);
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
                if (method.result.name == "Result" && method.result.arguments[1] != stringType &&
                    ((!method.pure && !objectClass(owner)) || (owner.kind == "Interface" && !owner.sourceInterface)))
                    fail("model actions and host methods require string Result errors", method.offset);
                if (isConcurrentCollection(method.result.name))
                    fail("methods cannot return concurrent collection handles", method.offset);
                if (isSynchronizationHandle(method.result.name))
                    fail("methods cannot return synchronization handles", method.offset);
                if (service(method.result) || method.result.name == "Atomic" || method.result.name == "CancellationToken")
                    fail("methods cannot return service references or cancellation tokens", method.offset);
                if (owner.kind == "Interface" && !owner.sourceInterface &&
                    !method.asynchronous && method.result.name != "Result") {
                    if (!method.pure)
                        fail("host interfaces declare synchronous Result<T,string> or Task<Result<T,string>> methods", method.offset);
                    if (!pureHostValue(method.result))
                        fail("pure host returns require deeply owned values without object references or handles", method.offset);
                }
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
                if (owner.kind == "Interface" && !owner.sourceInterface && method.pure &&
                    !method.asynchronous && method.result.name != "Result" && !pureHostValue(parameter.type))
                    fail("pure host arguments require deeply owned values without object references or handles", parameter.offset);
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
    Field* constant(Declaration& owner, const std::string& name) const {
        const auto found = std::find_if(owner.constants.begin(), owner.constants.end(), [&](const Field& value) {
            return value.name == name;
        });
        if (found != owner.constants.end())
            return &*found;
        if (!owner.baseClass.empty())
            return constant(*declaration(owner.baseClass), name);
        return nullptr;
    }
    void ensureConstant(Field& value, Declaration& owner) {
        if (constantStates_[&value] == 2)
            return;
        if (constantStates_[&value] == 1 || constantDepth_ == 64) {
            Diagnostic diagnostic(constantStates_[&value] == 1
                ? "constant dependency cycle" : "constant dependency depth exceeds 64", value.offset);
            diagnostic.module = owner.module;
            diagnostic.path = owner.originPath;
            throw diagnostic;
        }
        constantStates_[&value] = 1;
        Method initializer;
        initializer.pure = true;
        struct Context {
            Checker& checker;
            Declaration* owner;
            Method* method;
            bool checking;
            ~Context() {
                checker.owner_ = owner;
                checker.method_ = method;
                checker.checkingConstant_ = checking;
                --checker.constantDepth_;
            }
        } context{*this, owner_, method_, checkingConstant_};
        ++constantDepth_;
        owner_ = &owner;
        method_ = &initializer;
        checkingConstant_ = true;
        try {
            sameExpression(value.initializer, value.type);
            value.constantValue = foldConstant(value.initializer);
        } catch (Diagnostic& diagnostic) {
            if (diagnostic.module.empty())
                diagnostic.module = owner.module;
            if (diagnostic.path.empty())
                diagnostic.path = owner.originPath;
            throw;
        }
        constantStates_[&value] = 2;
    }
    Type constantExpression(const Expr& expression, Field& value) {
        auto& declaring = *declaration(value.declaringClass);
        if (!accessible(value.visible, value.protectedMember, declaring))
            fail("constant is private or unavailable", expression->offset);
        ensureConstant(value, declaring);
        expression->resolution = "constant";
        expression->constantValue = value.constantValue;
        expression->type = value.type;
        return value.type;
    }
    const Field* field(const Declaration& owner, const std::string& name) const {
        const auto found = std::find_if(owner.fields.begin(), owner.fields.end(), [&](const Field& value) {
            return value.name == name;
        });
        if (found != owner.fields.end())
            return &*found;
        const auto constant = std::find_if(owner.constants.begin(), owner.constants.end(), [&](const Field& value) {
            return value.name == name;
        });
        if (constant != owner.constants.end())
            return &*constant;
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
        if (!selected) {
            for (auto* candidatePointer : methods) {
                auto& candidate = *candidatePointer;
                if (candidate.name != name || candidate.parameters.size() != actual.size())
                    continue;
                bool matches = true;
                for (std::size_t index = 0; index < actual.size(); ++index)
                    matches = matches && assignable(actual[index], candidate.parameters[index].type);
                if (!matches)
                    continue;
                if (selected) {
                    const bool nullable = std::any_of(candidate.parameters.begin(), candidate.parameters.end(),
                        [](const Field& parameter) { return parameter.type.name == "Optional"; });
                    fail(nullable ? "ambiguous nullable method overload" : "ambiguous source interface upcast overload", node->offset);
                }
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
            fail("expected " + expected.displayText() + ", got " + actual.displayText(), offset);
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
        if (parallelDepth_ != 0 && !node->implicitObjectCall)
            fail("owner-confined object calls cannot enter parallel workers", node->offset);
        auto* receiver = declaration(receiverType.name);
        const auto& target = node->children[0];
        const auto* selected = preselected ? preselected : overload(*receiver, target->text, node);
        const auto& declaring = declaringMethod(*selected);
        if (parallelDepth_ != 0) {
            checkStatelessWorkerSelection(*selected, node->offset);
            node->statelessWorkerCall = true;
        }
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
        if (node->kind == "null") {
            result = {"$null", {}};
        } else if (node->kind == "integer") {
            std::int64_t parsed = 0;
            const auto conversion = std::from_chars(node->text.data(), node->text.data() + node->text.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != node->text.data() + node->text.size())
                fail("integer literal outside signed64 range", node->offset);
            result = integerType;
        } else if (node->kind == "uint32") {
            std::uint32_t parsed = 0;
            const bool hexadecimal = node->text.size() > 2 && node->text[0] == '0' &&
                (node->text[1] == 'x' || node->text[1] == 'X');
            const auto* begin = node->text.data() + (hexadecimal ? 2 : 0);
            const auto* end = node->text.data() + node->text.size() - 1;
            const auto conversion = std::from_chars(begin, end, parsed, hexadecimal ? 16 : 10);
            if (conversion.ec != std::errc{} || conversion.ptr != end)
                fail("uint32 literal outside range 0..4294967295", node->offset);
            node->resolution = std::to_string(parsed);
            result = unsignedType;
        } else if (node->kind == "float32") {

            float parsed = 0;
            const auto* end = node->text.data() + node->text.size() - 1;
            const auto conversion = std::from_chars(node->text.data(), end, parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != end || !std::isfinite(parsed))
                fail("float32 literal outside finite binary32 range", node->offset);
            node->resolution = std::to_string(std::bit_cast<std::uint32_t>(parsed));
            result = float32Type;
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
            if (auto* value = constant(*owner_, node->text))
                return constantExpression(node, *value);
            if (auto local = locals_.find(node->text); local != locals_.end()) {
                result = local->second;
                node->resolution = "local";
                if (parallelDepth_ != 0 && parallelOuter_.contains(node->text))
                    parallelUsed_.insert(node->text);
            } else if (const auto* found = field(*owner_, node->text)) {
                if (checkingConstant_)
                    fail("constant initializer cannot reference model state", node->offset);
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
                if (node->text == "!") {
                    same(result, booleanType, node->offset);
                } else if (node->text == "~") {
                    if (result != unsignedType)
                        fail("bitwise complement requires uint32", node->offset);
                } else if (result != integerType && result != floatType && result != float32Type) {
                    fail("negation requires int, float or float32", node->offset);
                }
            }
        } else if (node->kind == "binary") {
            auto left = expression(node->children[0]);
            auto right = expression(node->children[1]);
            const bool equality = node->text == "==" || node->text == "!=";
            if (equality && left.name == "$null" && right.name == "$null")
                fail("null requires a nullable target type", node->offset);
            if (equality && left.name == "Optional" && assignable(right, left)) {
                if (right != left)
                    node->children[1]->convertTo = left;
                right = left;
            } else if (equality && right.name == "Optional" && assignable(left, right)) {
                if (left != right)
                    node->children[0]->convertTo = right;
                left = right;
            }
            const bool referenceEquality = (node->text == "==" || node->text == "!=") &&
                objectReference(left) && objectReference(right);
            const auto& operation = node->text;
            const bool shift = operation == "<<" || operation == ">>";
            if (!referenceEquality && !shift)
                same(right, left, node->offset);
            if (shift) {
                if (left != unsignedType)
                    fail("shift requires a uint32 value", node->offset);
                same(right, integerType, node->offset);
                result = unsignedType;
            } else if (operation == "&" || operation == "|" || operation == "^") {
                if (left != unsignedType)
                    fail("bitwise operation requires uint32 operands", node->offset);
                result = unsignedType;
            } else if (operation == "&&" || operation == "||") {
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
            } else if (operation == "%") {
                if (left != integerType && left != unsignedType)
                    fail("remainder requires int or uint32 operands", node->offset);
                result = left;
            } else {
                if (left != integerType && left != unsignedType && left != floatType && left != float32Type)
                    fail("numeric operation requires int, uint32, float or float32", node->offset);
                result = operation == "+" || operation == "-" || operation == "*" || operation == "/"
                    ? left : booleanType;
            }
        } else if (node->kind == "conditional") {
            same(expression(node->children[0]), booleanType, node->children[0]->offset);
            result = expression(node->children[1]);
            const auto alternative = expression(node->children[2]);
            if (result.name == "$null" && alternative.name != "$null") {
                result = alternative.name == "Optional" ? alternative : Type{"Optional", {alternative}};
                type(result, node->offset);
                node->children[1]->convertTo = result;
                if (alternative != result)
                    node->children[2]->convertTo = result;
            } else if (alternative.name == "$null" && result.name != "$null") {
                if (result.name != "Optional")
                    result = Type{"Optional", {result}};
                type(result, node->offset);
                node->children[2]->convertTo = result;
                if (node->children[1]->type != result)
                    node->children[1]->convertTo = result;
            } else if (alternative != result && assignable(result, alternative)) {
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
            if (receiver->kind == "type" && receiver->type == cancellationType) {
                if (node->text != "None")
                    fail("unknown CancellationToken constant", node->offset);
                node->resolution = "cancellation_none";
                node->type = cancellationType;
                return cancellationType;
            }
            if (floatingEnvironmentReceiver(receiver))
                return floatingEnvironmentConstant(node);
            if (receiver->kind == "type" && (receiver->type == floatType || receiver->type == float32Type)) {
                if (node->text != "PositiveInfinity" && node->text != "NegativeInfinity" && node->text != "NaN")
                    fail("unknown typed IEEE constant", node->offset);
                result = receiver->type;
                node->resolution = "ieee_constant";
                node->type = result;
                return result;
            }
            const auto* named = receiver->kind == "name" ? declaration(receiver->text) : nullptr;
            if (named) {
                if (auto* value = constant(*declaration(named->name), node->text))
                    return constantExpression(node, *value);
            }
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
                    if (constant(*declaration(object->name), node->text))
                        fail("constant access requires its class name", node->offset);
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
                } else if (receiverType == byteBufferType) {
                    if (node->text != "Length")
                        fail("unknown ByteBuffer property", node->offset);
                    result = integerType;
                    node->resolution = "bytebuffer_length";
                } else if (receiverType == errorType) {
                    if (node->text != "Message")
                        fail("unknown Error property", node->offset);
                    result = stringType;
                    node->resolution = "error";
                } else if (receiverType.name == "List" || extended(receiverType.name) || isConcurrentCollection(receiverType.name)) {
                    if (node->text != "Count" &&
                        !((receiverType.name == "Array" || receiverType.name == "FixedArray") && node->text == "Length"))
                        fail("unknown List property", node->offset);
                    result = integerType;
                    node->resolution = "list";
                } else if (receiverType.name == "Optional") {
                    if (node->text == "HasValue")
                        result = booleanType;
                    else if (node->text == "Value")
                        result = receiverType.arguments[0];
                    else
                        fail("unknown nullable property", node->offset);
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
    bool floatingEnvironmentReceiver(const Expr& receiver) {
        return receiver->kind == "name" && receiver->text == "FloatingEnvironment" &&
            !locals_.contains(receiver->text) && !field(*owner_, receiver->text) && !declaration(receiver->text);
    }

    Type floatingEnvironmentConstant(const Expr& node) {
        namespace facts = pipelang::model::floating_environment;
        static const std::map<std::string, std::uint32_t> constants{
            {"PortableRoundingKnown", facts::portableRoundingKnown},
            {"PortableNearest", facts::portableNearest},
            {"PortableDown", facts::portableDown},
            {"PortableUp", facts::portableUp},
            {"PortableTowardZero", facts::portableTowardZero},
            {"Binary32RoundingKnown", facts::binary32RoundingKnown},
            {"Binary32Nearest", facts::binary32Nearest},
            {"Binary32Down", facts::binary32Down},
            {"Binary32Up", facts::binary32Up},
            {"Binary32TowardZero", facts::binary32TowardZero},
            {"Binary32InputsKnown", facts::binary32InputsKnown},
            {"Binary32GradualInputs", facts::binary32GradualInputs},
            {"Binary32ResultsKnown", facts::binary32ResultsKnown},
            {"Binary32GradualResults", facts::binary32GradualResults}
        };
        const auto found = constants.find(node->text);
        if (found == constants.end())
            fail("unknown FloatingEnvironment fact", node->offset);
        auto value = std::make_shared<Expression>();
        value->kind = "uint32";
        value->type = unsignedType;
        value->resolution = std::to_string(found->second);
        value->text = value->resolution + "u";
        node->resolution = "constant";
        node->constantValue = std::move(value);
        node->type = unsignedType;
        return unsignedType;
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
                node->implicitObjectCall = true;
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
        if (floatingEnvironmentReceiver(receiver)) {
            if (!program_.projectEnabled || !program_.objectEnabled)
                fail("FloatingEnvironment observation requires the project capability", node->offset);
            if (target->text != "Observe")
                fail("unknown FloatingEnvironment observation", target->offset);
            if (method_->pure || method_->asynchronous || parallelDepth_ != 0)
                fail("FloatingEnvironment.Observe requires a synchronous nonparallel effect", node->offset);
            arguments(node, {});
            node->resolution = "floating_environment_observe";
            return unsignedType;
        }
        auto* qualifiedOwner = receiver->kind == "name" &&
            !locals_.contains(receiver->text) && !field(*owner_, receiver->text)
            ? declaration(receiver->text) : nullptr;
        if (qualifiedOwner && qualifiedOwner->kind == "Class") {
            if (parallelDepth_ != 0)
                fail("qualified pure calls are not admitted in parallel workers", node->offset);
            // A fieldless exported model has a receiver-free pure facet. This
            // does not construct an actor or skip authored initialization policy.
            if (!program_.projectEnabled || !program_.objectEnabled ||
                !qualifiedOwner->modelExported || qualifiedOwner->generatedObjectFacet ||
                !qualifiedOwner->fields.empty() || !qualifiedOwner->baseClass.empty() ||
                !qualifiedOwner->interfaces.empty() || !qualifiedOwner->typeParameters.empty() ||
                !qualifiedOwner->genericSourceName.empty())
                fail("qualified pure calls require a fieldless nongeneric exported model", node->offset);
            for (const auto& candidate : qualifiedOwner->methods) {
                if (candidate.constructor && (!candidate.body.empty() || !candidate.parameters.empty() ||
                    candidate.baseConstructor || !candidate.fieldInitializers.empty()))
                    fail("qualified pure calls cannot bypass authored constructor policy", node->offset);
            }
            const auto* selected = overload(*qualifiedOwner, target->text, node);
            if (!selected || !selected->visible || !selected->pure || selected->asynchronous ||
                selected->constructor || selected->virtualMethod || selected->overrideMethod ||
                !selected->typeParameters.empty() || selected->sourceName.find('<') != std::string::npos)
                fail("qualified calls require a public synchronous nongeneric pure method", node->offset);
            node->resolution = "qualified_pure";
            node->owner = qualifiedOwner->name;
            node->callable = selected->identity;
            singleTarget(node, selected->identity);
            trackCalls(node->targets);
            return selected->result;
        }
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
            } else if (receiver->type.name == "FixedArray" && target->text == "Create") {
                parameters = {{receiver->type.arguments[0], "initial", true, node->offset}};
            } else if (receiver->type.name == "KeyValuePair" && target->text == "Create") {
                parameters = {{receiver->type.arguments[0], "key", true, node->offset},
                              {receiver->type.arguments[1], "value", true, node->offset}};
            } else if (receiver->type.name != "Array" && receiver->type.name != "FixedArray" &&
                       receiver->type.name != "KeyValuePair" && target->text == "Empty") {
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
        if (receiver->kind == "type" && receiver->type == byteBufferType) {
            if (target->text == "Empty") {
                arguments(node, {});
            } else if (target->text == "FromUtf8") {
                arguments(node, {{stringType, "text", true, node->offset}});
            } else {
                fail("unknown ByteBuffer constructor", target->offset);
            }
            node->resolution = "collection_construct";
            return byteBufferType;
        }
        if (receiver->kind == "type" && receiver->type == stringType) {
            if (target->text == "ByteLengthIs") {
                arguments(node, {{stringType, "value", true, node->offset}, {integerType, "expected", true, node->offset}});
                node->resolution = "string_byte_length_is";
                return booleanType;
            }
            if (target->text == "ByteAtOr") {
                arguments(node, {{stringType, "value", true, node->offset}, {integerType, "index", true, node->offset},
                    {integerType, "fallback", true, node->offset}});
                node->resolution = "string_byte_at_or";
                return integerType;
            }
            if (target->text == "StartsWith") {
                arguments(node, {{stringType, "value", true, node->offset}, {stringType, "prefix", true, node->offset}});
                node->resolution = "string_starts_with";
                return booleanType;
            }
            if (target->text == "FromInt") {
                arguments(node, {{integerType, "value", true, node->offset}});
                node->resolution = "string_from_int";
                return stringType;
            }
            if (target->text == "Utf8Length") {
                arguments(node, {{stringType, "value", true, node->offset}});
                node->resolution = "string_utf8_length";
            } else if (target->text == "Utf8Slice") {
                arguments(node, {{stringType, "value", true, node->offset},
                                 {integerType, "start", true, node->offset},
                                 {integerType, "count", true, node->offset}});
                node->resolution = "string_utf8_slice";
                return stringType;
            } else if (target->text == "Utf8IndexOf") {
                arguments(node, {{stringType, "value", true, node->offset},
                                 {stringType, "pattern", true, node->offset},
                                 {integerType, "start", true, node->offset}});
                node->resolution = "string_utf8_index_of";
            } else {
                fail("unknown string operation", target->offset);
            }
            return integerType;
        }
        if (receiver->kind == "type" && receiver->type == unsignedType) {
            if (target->text == "WrappingAdd" || target->text == "WrappingSubtract" || target->text == "WrappingMultiply") {
                arguments(node, {{unsignedType, "left", true, node->offset}, {unsignedType, "right", true, node->offset}});
                node->resolution = "unsigned_wrapping";
                return unsignedType;
            }
            Type input;
            if (target->text == "FromInt") {
                input = integerType;
            } else if (target->text == "FromFloat") {
                input = floatType;
            } else if (target->text == "FromFloat32") {
                input = float32Type;
            } else {
                fail("unknown uint32 operation", target->offset);
            }
            arguments(node, {{input, "value", true, node->offset}});
            node->resolution = "unsigned_conversion";
            return unsignedType;
        }
        if (receiver->kind == "type" && receiver->type == integerType) {
            if (target->text == "RoundFromFloat" || target->text == "RoundFromFloat32") {
                arguments(node, {{target->text == "RoundFromFloat" ? floatType : float32Type, "value", true, node->offset}});
                node->resolution = "numeric_round";
                return integerType;
            }
            if (target->text == "FromUInt32") {
                arguments(node, {{unsignedType, "value", true, node->offset}});
                node->resolution = "unsigned_conversion";
                return integerType;
            }

            Type input;
            if (target->text == "FromFloat") {
                input = floatType;
            } else if (target->text == "FromFloat32") {
                input = float32Type;
            } else {
                fail("unknown int operation", target->offset);
            }
            arguments(node, {{input, "value", true, node->offset}});
            node->resolution = "numeric_conversion";
            return integerType;
        }
        if (receiver->kind == "type" && (receiver->type == floatType || receiver->type == float32Type)) {
            if (receiver->type == float32Type && (target->text == "ToBits" || target->text == "FromBits")) {
                const bool toBits = target->text == "ToBits";
                arguments(node, {{toBits ? float32Type : unsignedType, "value", true, node->offset}});
                node->resolution = "float_bits";
                return toBits ? unsignedType : float32Type;
            }
            if (target->text == "FromUInt32") {
                arguments(node, {{unsignedType, "value", true, node->offset}});
                node->resolution = "unsigned_conversion";
                return receiver->type;
            }
            if (target->text == "FromInt" ||

                (receiver->type == floatType && target->text == "FromFloat32") ||
                (receiver->type == float32Type && target->text == "FromFloat")) {
                Type input = integerType;
                if (target->text == "FromFloat32")
                    input = float32Type;
                if (target->text == "FromFloat")
                    input = floatType;
                arguments(node, {{input, "value", true, node->offset}});
                node->resolution = "numeric_conversion";
                return receiver->type;
            }
            if (target->text == "Clamp") {
                arguments(node, {{receiver->type, "value", true, node->offset},
                    {receiver->type, "minimum", true, node->offset},
                    {receiver->type, "maximum", true, node->offset}});
                node->resolution = "float_math";
                return receiver->type;
            }
            if (target->text == "ILogB" || target->text == "ScaleB") {
                std::vector<Field> parameters{{receiver->type, "value", true, node->offset}};
                if (target->text == "ScaleB")
                    parameters.push_back({integerType, "exponent", true, node->offset});
                arguments(node, parameters);
                node->resolution = "float_math";
                return target->text == "ILogB" ? integerType : receiver->type;
            }
            static const std::set<std::string> unary{
                "IsFinite", "Abs", "Sqrt", "Log", "Sin", "Cos", "Tan", "Asin", "Acos", "Atan",
                "Exp", "Expm1", "Cbrt", "Tanh", "Floor", "Ceiling"
            };
            static const std::set<std::string> binary{
                "Atan2", "Pow", "Hypot", "Fmod", "Remainder", "CopySign"
            };
            if (!unary.contains(target->text) && !binary.contains(target->text))
                fail("unknown " + receiver->type.name + " operation", target->offset);
            std::vector<Field> parameters{{receiver->type, "value", true, node->offset}};
            if (binary.contains(target->text))
                parameters.push_back({receiver->type, "other", true, node->offset});
            if (target->text == "Hypot" && node->children.size() == 4)
                parameters.push_back({receiver->type, "third", true, node->offset});
            arguments(node, parameters);
            node->resolution = "float_math";
            return target->text == "IsFinite" ? booleanType : receiver->type;
        }
        if (receiver->kind == "type" && receiver->type.name == "Optional")
            fail("nullable values use typed values or null, not Some/None constructors", target->offset);
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
        if (receiverType == byteBufferType) {
            std::vector<Field> parameters;
            Type result = byteBufferType;
            std::string operation;
            bool mutating = false;
            if (target->text == "At") {
                parameters = {{integerType, "index", true, node->offset}};
                result = unsignedType;
                operation = "at";
            } else if (target->text == "Slice") {
                parameters = {{integerType, "start", true, node->offset}, {integerType, "count", true, node->offset}};
                operation = "slice";
            } else if (target->text == "IndexOf") {
                parameters = {{byteBufferType, "pattern", true, node->offset}, {integerType, "start", true, node->offset}};
                result = integerType;
                operation = "indexOf";
            } else if (target->text == "DecodeUtf8") {
                result = stringType;
                operation = "decodeUtf8";
            } else if (target->text == "Append" || target->text == "Add") {
                parameters = {{target->text == "Add" ? unsignedType : byteBufferType, "value", true, node->offset}};
                result = voidType;
                operation = target->text == "Add" ? "add" : "append";
                mutating = true;
            } else {
                fail("unknown ByteBuffer operation", target->offset);
            }
            if (mutating)
                assignmentTarget(receiver);
            arguments(node, parameters);
            node->resolution = std::string(mutating ? "collection_mutate_" : "collection_read_") + operation;
            return result;
        }
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
        if (!host || host->kind != "Interface" || host->sourceInterface)
            fail("unknown host service receiver", node->offset);
        auto* mutableHost = declaration(receiverType.name);
        const auto* found = overload(mutableHost->methods, target->text, node);
        if (!found->visible)
            fail("host interface method is private", target->offset);
        if (found->asynchronous && !allowServiceCall)
            fail("host calls must be directly awaited in an async method", node->offset);
        if (method_->pure && !found->pure)
            fail("pure methods cannot call effectful host services", node->offset);
        if (parallelDepth_ != 0)
            fail("parallel workers cannot access host services", node->offset);
        if (found->asynchronous)
            node->resolution = "host";
        else if (found->pure && found->result.name != "Result")
            node->resolution = "host_sync_value";
        else
            node->resolution = "host_sync";
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
        if ((receiver.name == "Array" || receiver.name == "FixedArray" || dictionary) && name == "At") {
            parameters = {dictionary ? element : integerType};
            result = value;
            operation = "at";
        } else if ((receiver.name == "Array" || receiver.name == "FixedArray" || dictionary) && name == "Set") {
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
        } else if (receiver.name != "Array" && receiver.name != "FixedArray" && receiver.name != "KeyValuePair" && name == "Clear") {
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
    void checkStatelessWorkerSelection(const Method& target, std::size_t offset) {
        const auto& declaring = declaringMethod(target);
        if (!target.pure || target.asynchronous || target.constructor || target.virtualMethod ||
            target.overrideMethod || declaring.name != owner_->name || !declaring.baseClass.empty() ||
            !target.typeParameters.empty() || target.sourceName.find('<') != std::string::npos)
            fail("parallel object helper requires an implicit same-class nonvirtual nongeneric pure method", offset);
    }

    bool statelessWorkerValue(const Type& value) {
        if (referenceBearing(value) || service(value) || isSharedHandle(value) ||
            value.name == "Task" || value.name == "CancellationToken")
            return false;
        for (const auto& argument : value.arguments) {
            if (!statelessWorkerValue(argument))
                return false;
        }
        const auto* record = declaration(value.name);
        if (record && record->kind == "Record") {
            for (const auto& field : record->fields) {
                if (!statelessWorkerValue(field.type))
                    return false;
            }
        }
        return true;
    }

    void checkStatelessObjectWorker(Method& target, std::set<std::string>& checked) {
        if (!checked.insert(target.identity).second)
            return;
        OwnerContext scope(owner_, methodOwner(target));
        checkStatelessWorkerSelection(target, target.offset);
        if (!statelessWorkerValue(target.result))
            fail("parallel object helper requires deeply owned value results", target.offset);
        for (const auto& parameter : target.parameters) {
            if (!statelessWorkerValue(parameter.type))
                fail("parallel object helper requires deeply owned value arguments", parameter.offset);
        }

        std::function<void(const Expr&)> inspectExpression;
        std::function<void(const std::vector<Statement>&)> inspectStatements;
        inspectExpression = [&](const Expr& node) {
            if (!node)
                return;
            if (node->resolution == "object_call" && node->implicitObjectCall) {
                auto& callee = *method(node->callable);
                checkStatelessWorkerSelection(callee, node->offset);
                checkStatelessObjectWorker(callee, checked);
                node->statelessWorkerCall = true;
                // The synthetic `this` is not a value capture. The admitted
                // worker function has no receiver; all authored arguments still
                // pass the same transitive value/effect checks in source order.
                for (std::size_t index = 1; index < node->children.size(); ++index)
                    inspectExpression(node->children[index]);
                return;
            }
            if (node->resolution == "floating_environment_observe")
                fail("parallel helpers cannot observe the floating environment", node->offset);
            if (node->resolution == "qualified_pure")
                fail("parallel helpers cannot use qualified pure calls", node->offset);
            if (!statelessWorkerValue(node->type) || node->resolution == "field" ||
                node->resolution == "object_field" || node->resolution == "this" ||
                node->resolution == "base" || node->resolution == "service" ||
                node->resolution == "host" || node->resolution == "host_sync" ||
                node->resolution == "host_sync_value" || node->resolution == "local_async" ||
                node->kind == "await")
                fail("parallel object helper cannot access state, references, host services or suspension", node->offset);
            for (const auto& child : node->children)
                inspectExpression(child);
        };
        inspectStatements = [&](const std::vector<Statement>& body) {
            for (const auto& statement : body) {
                if (statement.kind == "parallel_foreach" || statement.kind == "lock")
                    fail("parallel object helper cannot start nested parallel or synchronized work", statement.offset);
                if (!statelessWorkerValue(statement.type))
                    fail("parallel object helper locals require deeply owned values", statement.offset);
                inspectExpression(statement.expression);
                inspectExpression(statement.target);
                inspectStatements(statement.initialization);
                inspectStatements(statement.iteration);
                inspectStatements(statement.body);
                inspectStatements(statement.alternative);
                inspectStatements(statement.cleanup);
            }
        };
        inspectStatements(target.body);
        target.statelessWorkerCallable = true;
    }

    void checkWorkerCalls(Declaration& owner) {
        std::set<std::string> checked;
        std::set<std::string> statelessChecked;
        std::function<void(Method&)> worker;
        std::function<void(const Expr&)> inspectExpression;
        std::function<void(const std::vector<Statement>&)> inspectStatements;
        inspectExpression = [&](const Expr& node) {
            if (!node)
                return;
            if (node->resolution == "floating_environment_observe")
                fail("parallel helpers cannot observe the floating environment", node->offset);
            if (node->resolution == "qualified_pure")
                fail("parallel helpers cannot use qualified pure calls", node->offset);
            if (referenceBearing(node->type) || node->resolution == "this" || node->resolution == "object_field")
                fail("parallel helper cannot access owner-confined objects", node->offset);
            if (node->resolution == "service" || node->resolution == "host" || node->resolution == "host_sync" || node->resolution == "local_async" || node->kind == "await")
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
                inspectStatements(statement.initialization);
                inspectStatements(statement.iteration);
                inspectStatements(statement.body);
                inspectStatements(statement.alternative);
                inspectStatements(statement.cleanup);
            }
        };
        worker = [&](Method& target) {
            if (objectClass(*methodOwner(target))) {
                checkStatelessObjectWorker(target, statelessChecked);
                return;
            }
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
                    inspect(statement.initialization);
                    inspect(statement.iteration);
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
                if (context == Context::Async && node->resolution == "floating_environment_observe")
                    fail("async call closures cannot observe the floating environment", node->offset);
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
                    inspectStatements(statement.initialization);
                    inspectStatements(statement.iteration);
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
        if (target->resolution == "constant")
            fail("cannot assign to const member", target->offset);
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
            } else if (statement.kind == "for") {
                if (statement.initialization.size() > 1 || statement.iteration.size() > 1 || !statement.expression)
                    fail("for requires a single initializer, condition and iteration clause", statement.offset);
                for (const auto& initializer : statement.initialization) {
                    if (initializer.kind != "local" && initializer.kind != "assign" && initializer.kind != "expression")
                        fail("for initialization requires a local declaration, assignment or method call", initializer.offset);
                }
                for (const auto& iteration : statement.iteration) {
                    if (iteration.kind != "assign" && iteration.kind != "expression")
                        fail("for iteration requires an assignment or method call", iteration.offset);
                    if (iteration.expression && iteration.expression->kind == "await")
                        fail("for iteration cannot suspend", iteration.offset);
                }
                auto outer = locals_;
                const auto initializationFlow = statements(statement.initialization);
                same(expression(statement.expression), booleanType, statement.offset);
                const auto headerScope = locals_;
                ++loopDepth_;
                const auto loopFlow = statements(statement.body);
                // Body declarations end before the iteration clause. The
                // initializer's locals remain available until the loop exits.
                locals_ = headerScope;
                const auto iterationFlow = statements(statement.iteration);
                --loopDepth_;
                locals_ = std::move(outer);
                // As with while, even a constant condition does not establish
                // a return. Loop-local break/continue are consumed here.
                next = Fallthrough | ((initializationFlow | loopFlow | iterationFlow) & (Returned | Thrown));
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
            if (type.name == "FixedArray") {
                const auto child = visitType(type.arguments.front(), offset);
                layout.depth = child.depth + 1;
                const auto extent = *type.fixedExtent;
                if (extent != 0) {
                    // Saturate before multiplication. A containing record's
                    // established 256-node bound owns the eventual rejection.
                    if (child.leaves > maximumLeaves || extent > (maximumLeaves - 1) / child.leaves)
                        layout.leaves = maximumLeaves + 1;
                    else
                        layout.leaves += extent * child.leaves;
                }
                return layout;
            }
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
            const bool localCall = node->resolution == "pure" || node->resolution == "qualified_pure" ||
                node->resolution == "effect" || node->resolution == "local_async";
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
                work = addCost(work, statementCost(statement.initialization), statement.offset);
                work = addCost(work, statementCost(statement.iteration), statement.offset);
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
    std::map<const Field*, unsigned> constantStates_;
    unsigned constantDepth_ = 0;
    bool checkingConstant_ = false;
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
