// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"
#include "AsyncFinallyLowering.hpp"
#include "CompiledImplementationUnits.hpp"
#include "NativeNumericExports.hpp"
#include "ProgramTypeUsage.hpp"
#include "CompositionEmitter.hpp"
#include "CompositionPlanner.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <sstream>

namespace pipelang::models {
namespace {

const Declaration* findType(const Program& program, const std::string& name) {
    const auto found = std::find_if(program.declarations.begin(), program.declarations.end(), [&](const Declaration& value) {
        return value.name == name;
    });
    return found == program.declarations.end() ? nullptr : &*found;
}

bool service(const Program& program, const Type& type) {
    const auto* declaration = findType(program, type.name);
    return declaration && declaration->kind == "Interface" && !declaration->sourceInterface;
}

std::string escaped(const std::string& text) {
    // Emit every source byte as an octal escape. Embedded NUL and UTF-8 are
    // preserved with an explicit byte count, independent of native source locale.
    std::string result = "std::string(\"";
    for (unsigned char byte : text) {
        result += '\\';
        result += static_cast<char>('0' + ((byte >> 6) & 7));
        result += static_cast<char>('0' + ((byte >> 3) & 7));
        result += static_cast<char>('0' + (byte & 7));
    }
    return result + "\", " + std::to_string(text.size()) + ")";
}

class Generator {
public:
    explicit Generator(const Program& program, bool compiled = false)
        : program_(program), compiled_(compiled) {}

    std::string compositionFactory(const std::string& nameSpace) {
        nameSpace_ = nameSpace;
        prepareObjectNames();
        const CompositionFactoryBindings bindings{
            nameSpace, compiled_,
            [this](const Type& type) { return cpp(type); },
            [this](const std::string& identity) { return objectFunction(identity); },
            [this](const Type& type) { return objectTag(type.name); }
        };
        return emitCompositionFactory(program_, planCompositions(program_), bindings);
    }

    std::string runNumericHeader(const std::string& nameSpace) {
        standaloneHeader_ = true;
        numericEmission_ = true;
        nameSpace_ = nameSpace + "::numeric_header";
        line("// Generated caller-host numeric projection; ordinary model types remain separate.");
        line("#pragma once");
        line("#include <array>");
        if (usesType("FixedArray"))
            line("#include <pipelang/model/fixed_array.hpp>");
        line("#include <bit>");
        line("#include <pipelang/model/result.hpp>");
        if (usesType("uint32"))
            line("#include <pipelang/model/uint32.hpp>");
        if (usesType("float") || usesType("float32"))
            line("#include <pipelang/model/numeric.hpp>");

        if (usesType("string"))
            line("#include <pipelang/model/byte_text.hpp>");
        line("namespace " + nameSpace_ + " {");
        std::set<std::string> valueDeclarations;
        for (const auto& owner : program_.declarations) {
            for (const auto& method : owner.methods) {
                NativeNumericExports::Projection projection;
                if (admitNumeric(owner, method, false, &projection))
                    valueDeclarations.insert(projection.valueDeclarations.begin(), projection.valueDeclarations.end());
            }
        }
        for (const auto& declaration : program_.declarations) {
            if (!valueDeclarations.contains(declaration.name) || declaration.kind != "Enum")
                continue;
            std::string values;
            for (const auto& value : declaration.values) {
                if (!values.empty())
                    values += ", ";
                values += value;
            }
            line("enum class " + declaration.name + " { " + values + " };");
            emitEnumValidation(declaration);
        }
        std::set<std::string> emittedValueRecords;
        std::function<void(const Type&)> emitValueType;
        std::function<void(const Declaration&)> emitValueRecord;
        emitValueType = [&](const Type& type) {
            for (const auto& argument : type.arguments)
                emitValueType(argument);
            const auto* declaration = findType(program_, type.name);
            if (declaration && declaration->kind == "Record" && valueDeclarations.contains(declaration->name))
                emitValueRecord(*declaration);
        };
        emitValueRecord = [&](const Declaration& declaration) {
            if (emittedValueRecords.contains(declaration.name))
                return;
            for (const auto& field : declaration.fields)
                emitValueType(field.type);
            line("struct " + declaration.name + " {");
            for (const auto& field : declaration.fields) {
                const auto initializer = field.initializer ? " = " + defaultExpression(field.initializer) : "{}";
                line(cpp(field.type) + " " + field.name + initializer + ";", 1);
            }
            line("bool operator==(const " + declaration.name + "&) const = default;", 1);
            line("};");
            if (usesType("FixedArray"))
                emitRecordValueOperations(declaration, true);
            emittedValueRecords.insert(declaration.name);
        };
        for (const auto& declaration : program_.declarations) {
            if (valueDeclarations.contains(declaration.name) && declaration.kind == "Record")
                emitValueRecord(declaration);
        }
        for (const auto& declaration : program_.declarations) {
            if (!valueDeclarations.contains(declaration.name))
                continue;
            const auto [aliasNamespace, typeName] = sourceAliasLocation(declaration.sourceName);
            line("namespace " + aliasNamespace + " { using " + typeName + " = " + declaration.name + "; }");
        }
        emitNativeNumericExports();
        line("}");
        return output_.str();
    }

    std::map<std::string, std::string> implementationFiles() const {
        auto files = implementationUnits_->files();
        files.emplace("Objects.hpp", objects_.str());
        return files;
    }
    std::string run(const std::string& nameSpace, bool typesOnly = false) {
        nameSpace_ = nameSpace;
        line(program_.objectEnabled ? "// Generated by pipelang-modelc; native-model object capability v3."
                                    : "// Generated by pipelang-modelc; native-model capability v1.");
        line("#pragma once");
        line("#include <array>");
        if (usesType("FixedArray"))
            line("#include <pipelang/model/fixed_array.hpp>");
        if (usesType("string"))
            line("#include <pipelang/model/byte_text.hpp>");
        line("#include <pipelang/model/floating_environment.hpp>");
        if (program_.activeHostNumericEnvironment && !typesOnly)
            line("#include <pipelang/model/active_numeric.hpp>");
        if (program_.projectEnabled && !typesOnly) {
            prepareObjectNames();
            line("#include \"Types.hpp\"");
            line("");
            line("namespace " + nameSpace + " {");
        } else {
            if (!constantsManifest(program_, nameSpace).empty())
                output_ << constantsHeader(program_, nameSpace);
            line("#include <pipelang/model/runtime.hpp>");
            if (program_.objectEnabled)
                line("#include <pipelang/model/object_model.hpp>");
            if (usesType("uint32"))
                line("#include <pipelang/model/uint32.hpp>");
            if (usesType("float32"))

                line("#include <pipelang/model/binary32.hpp>");
            if (program_.projectEnabled && usesType("float32"))
                line("#include <pipelang/model/binary32_operations.hpp>");
            if (usesType("float") || usesType("float32"))
                line("#include <pipelang/model/numeric.hpp>");
            if (usesType("List") || usesLoops() || usesLocalAsync() || usesFailureControl())
                line("#include <pipelang/model/collection.hpp>");
            if (usesFailureControl())
                line("#include <pipelang/model/cleanup.hpp>");
            if (usesExtendedCollections())
                line("#include <pipelang/model/extended_collections.hpp>");
            if (usesSynchronization())
                line("#include <pipelang/model/synchronization.hpp>");
            if (usesType("ContextOwner"))
                line("#include <pipelang/model/context.hpp>");
            if (usesConcurrentCollections())
                line("#include <pipelang/model/concurrent_collections.hpp>");
            if (usesType("Atomic"))
                line("#include <pipelang/model/atomic.hpp>");
            if (usesParallel())
                line("#include <pipelang/model/parallel.hpp>");
            line("#include <cstdint>");
            line("#include <functional>");
            line("#include <memory>");
            line("#include <string>");
            line("#include <utility>");
            if (program_.projectEnabled)
                line("#include <type_traits>");
            line("");
            line("namespace " + nameSpace + " {");
            for (const auto& declaration : program_.declarations) {
                if (declaration.kind == "Enum") {
                    std::string values;
                    for (const auto& value : declaration.values) {
                        if (!values.empty())
                            values += ", ";
                        values += value;
                    }
                    line("enum class " + declaration.name + " { " + values + " };");
                    emitEnumValidation(declaration);
                }
            }
            emitObjectTags();
            std::set<std::string> emitted;
            std::function<void(const Type&)> emitType;
            std::function<void(const Declaration&)> emitRecord;
            emitType = [&](const Type& type) {
                for (const auto& argument : type.arguments)
                    emitType(argument);
                const auto* declaration = findType(program_, type.name);
                if (declaration && declaration->kind == "Record")
                    emitRecord(*declaration);
            };
            emitRecord = [&](const Declaration& record) {
                if (emitted.contains(record.name))
                    return;
                for (const auto& field : record.fields)
                    emitType(field.type);
                line("struct " + record.name + " {");
                for (const auto& field : record.fields) {
                    const auto initializer = field.initializer ? " = " + defaultExpression(field.initializer) : "{}";
                    line("    " + cpp(field.type) + " " + field.name + initializer + ";");
                }
                line("    bool operator==(const " + record.name + "&) const = default;");
                line("};");
                emitRecordValueOperations(record, program_.objectEnabled || usesType("List") || usesExtendedCollections());
                emitted.insert(record.name);
            };
            for (const auto& declaration : program_.declarations) {
                if (declaration.kind == "Record")
                    emitRecord(declaration);
            }
            for (const auto& declaration : program_.declarations) {
                if (declaration.kind == "Interface" && !declaration.sourceInterface)
                    interface(declaration);
            }
        }
        if (typesOnly) {
            emitSourceAliases(false);
            line("}");
            return output_.str();
        }
        prepareQualifiedFunctions();
        prepareObservationMethods();
        emitQualifiedDeclarations();
        if (compiled_) {
            objects_ << "// Generated private project declarations.\n#pragma once\n"
                     << "#include \"Model.hpp\"\nnamespace " << nameSpace << " {\n";
            implementationUnits_.emplace(nameSpace);
            for (const auto& declaration : program_.declarations) {
                if (declaration.kind == "Class" || declaration.sourceInterface)
                    implementationUnits_->addOwner(declaration.name);
            }
            selectedOutput_ = &objects_;
        }
        emitObjectDefinitions();
        selectedOutput_ = &output_;
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind == "Class" && (!program_.objectEnabled || declaration.modelExported))
                model(declaration);
        }
        emitQualifiedDefinitions();
        if (program_.projectEnabled) {
            emitSourceAliases(true);
            emitNativeNumericExports();
            emitNativeObservationExports();
        }
        if (compiled_)
            objects_ << "}\n";
        line("}");
        return output_.str();
    }
private:
    bool supportsConstantValidation(const Type& type, std::set<std::string>& records) const {
        if (type.arguments.empty() && (type.name == "bool" || type.name == "int" ||
            type.name == "uint32" || type.name == "float" || type.name == "float32"))
            return true;
        if (type.name == "Optional" || type.name == "FixedArray" || type.name == "Result") {
            return std::all_of(type.arguments.begin(), type.arguments.end(), [&](const Type& argument) {
                return argument.name == "void" || supportsConstantValidation(argument, records);
            });
        }
        const auto* declaration = findType(program_, type.name);
        if (!declaration || !type.arguments.empty())
            return false;
        if (declaration->kind == "Enum")
            return true;
        if (declaration->kind != "Record" || !records.insert(type.name).second)
            return false;
        const bool supported = std::all_of(declaration->fields.begin(), declaration->fields.end(), [&](const Field& field) {
            return supportsConstantValidation(field.type, records);
        });
        records.erase(type.name);
        return supported;
    }

    void emitEnumValidation(const Declaration& declaration) {
        line("inline constexpr void validate(" + declaration.name + " value) {");
        line("    switch (value) {");
        for (const auto& value : declaration.values)
            line("    case " + declaration.name + "::" + value + ": return;");
        line("    }");
        line("    throw pipelang::model::ExecutionFailure(\"invalid enum value\");");
        line("}");
    }

    void emitRecordValueOperations(const Declaration& record, bool ownership) {
        std::set<std::string> records;
        const std::string qualifier = supportsConstantValidation(Type{record.name, {}}, records)
            ? "inline constexpr " : "inline ";
        line(qualifier + "void validate(const " + record.name + "& value) {");
        line("    using pipelang::model::validate;");
        if (record.fields.empty())
            line("    (void)value;");
        for (const auto& field : record.fields)
            line("    validate(value." + field.name + ");");
        line("}");
        if (!ownership)
            return;
        line(qualifier + "std::size_t ownedBytes(const " + record.name + "& value) {");
        line("    using pipelang::model::ownedBytes;");
        line("    std::size_t bytes = sizeof(value);");
        for (const auto& field : record.fields)
            line("    bytes = pipelang::model::addOwnedBytes(bytes, ownedBytes(value." + field.name + ")); ");
        line("    return bytes;");
        line("}");
        line(qualifier + "std::size_t ownedElements(const " + record.name + "& value) {");
        line("    using pipelang::model::ownedElements;");
        line("    std::size_t elements = 0;");
        if (record.fields.empty())
            line("    (void)value;");
        for (const auto& field : record.fields)
            line("    elements = pipelang::model::boundedOwnedSum(elements, ownedElements(value." + field.name + "), pipelang::model::maximumListElements);");
        line("    return elements;");
        line("}");
    }

    std::pair<std::string, std::string> sourceAliasLocation(const std::string& identity) const {
        const auto separator = identity.rfind('.');
        if (separator == std::string::npos)
            throw Diagnostic("project export requires a module-qualified identity", 0);
        std::string nameSpace = "source::";
        for (const auto character : identity.substr(0, separator)) {
            if (character == '.')
                nameSpace += "::";
            else
                nameSpace += character;
        }
        return {nameSpace, identity.substr(separator + 1)};
    }

    void emitSourceAliases(bool nativeRootsOnly) {
        std::set<std::string> namespaces;
        std::set<std::string> types;
        for (const auto& declaration : program_.declarations) {
            if (declaration.genericSourceName.empty())
                types.insert(declaration.sourceName);
        }
        for (const auto& [identity, arity] : program_.projectGenericRecords) {
            (void)arity;
            types.insert(identity);
        }
        for (const auto& identity : types) {
            if (identity.empty())
                throw Diagnostic("project export is missing a source identity", 0);
            for (auto separator = identity.find('.'); separator != std::string::npos;
                 separator = identity.find('.', separator + 1))
                namespaces.insert(identity.substr(0, separator));
        }
        for (const auto& identity : types) {
            if (namespaces.contains(identity))
                throw Diagnostic("source type conflicts with a generated module namespace: " + identity, 0);
        }
        for (const auto& [identity, arity] : program_.projectGenericRecords) {
            if (nativeRootsOnly)
                continue;
            const auto [nameSpace, typeName] = sourceAliasLocation(identity);
            std::string parameters;
            std::string arguments;
            for (std::size_t index = 0; index < arity; ++index) {
                if (index != 0) {
                    parameters += ", ";
                    arguments += ", ";
                }
                const auto parameter = "pl_argument_" + std::to_string(index);
                parameters += "typename " + parameter;
                arguments += parameter;
            }
            line("namespace " + nameSpace + " {");
            line("template<" + parameters + "> struct pl_record_" + typeName + ";");
            line("template<" + parameters + "> using " + typeName + " = typename pl_record_" + typeName + "<" + arguments + ">::type;");
            line("}");
        }
        for (const auto& declaration : program_.declarations) {
            if (declaration.generatedObjectFacet)
                continue;
            const bool nativeRoot = declaration.kind == "Class" && declaration.modelExported;
            if (nativeRoot != nativeRootsOnly)
                continue;
            const auto nativeType = "::" + nameSpace_ + "::" + declaration.name;
            if (!declaration.genericSourceName.empty()) {
                const auto [nameSpace, typeName] = sourceAliasLocation(declaration.genericSourceName);
                std::string arguments;
                for (const auto& argument : declaration.genericArguments) {
                    if (!arguments.empty())
                        arguments += ", ";
                    arguments += cpp(argument);
                }
                line("namespace " + nameSpace + " {");
                line("template<> struct pl_record_" + typeName + "<" + arguments + "> {");
                line("using type = " + nativeType + ";");
                line("};");
                line("}");
                continue;
            }
            const auto [nameSpace, typeName] = sourceAliasLocation(declaration.sourceName);
            auto aliasType = nativeType;
            if (objectClass(declaration) || declaration.sourceInterface)
                aliasType = "pipelang::model::ObjectRef<::" + nameSpace_ + "::" + objectTag(declaration.name) + ">";
            line("namespace " + nameSpace + " {");
            line("using " + typeName + " = " + aliasType + ";");
            line("}");
        }
    }

    struct QualifiedFunction {
        const Declaration* owner;
        const Method* method;
        std::string name;
    };

    const Declaration& methodOwner(const std::string& identity) const {
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                if (method.identity == identity)
                    return declaration;
            }
        }
        throw Diagnostic("qualified pure call has no resolved owner", 0);
    }

    const Method& resolvedMethod(const Declaration& owner, const std::string& identity) const {
        const auto found = std::find_if(owner.methods.begin(), owner.methods.end(), [&](const Method& method) {
            return method.identity == identity;
        });
        if (found == owner.methods.end())
            throw Diagnostic("qualified pure call has no resolved method", 0);
        return *found;
    }

    void prepareQualifiedFunctions() {
        std::vector<const Method*> pending;
        std::function<void(const Expr&, bool)> inspectExpression;
        std::function<void(const std::vector<Statement>&, bool)> inspectStatements;
        inspectExpression = [&](const Expr& node, bool helperBody) {
            if (!node)
                return;
            if (node->resolution == "qualified_pure" || (helperBody && node->resolution == "pure")) {
                if (!qualifiedFunctions_.contains(node->callable)) {
                    const auto& owner = methodOwner(node->callable);
                    const auto& method = resolvedMethod(owner, node->callable);
                    const auto name = "pl_qualified_" + std::to_string(qualifiedFunctions_.size());
                    qualifiedFunctions_.emplace(node->callable, QualifiedFunction{&owner, &method, name});
                    if (program_.objectEnabled)
                        pending.push_back(&method);
                    else
                        inspectStatements(method.body, true);
                }
            }
            for (const auto& child : node->children)
                inspectExpression(child, helperBody);
        };
        inspectStatements = [&](const std::vector<Statement>& body, bool helperBody) {
            for (const auto& statement : body) {
                inspectExpression(statement.expression, helperBody);
                inspectExpression(statement.target, helperBody);
                inspectStatements(statement.initialization, helperBody);
                inspectStatements(statement.body, helperBody);
                inspectStatements(statement.iteration, helperBody);
                inspectStatements(statement.alternative, helperBody);
                inspectStatements(statement.cleanup, helperBody);
            }
        };
        for (const auto& owner : program_.declarations) {
            for (const auto& method : owner.methods)
                inspectStatements(method.body, false);
        }
        while (!pending.empty()) {
            const auto* method = pending.back();
            pending.pop_back();
            inspectStatements(method->body, true);
        }
    }

    std::string qualifiedParameters(const Method& method) const {
        std::string result = "pipelang::model::EvaluationBudget& pl_budget";
        if (program_.objectEnabled)
            result += ", pipelang::model::ObjectExecution& pl_parentExecution";
        const auto authored = parameters(method);
        if (!authored.empty())
            result += ", " + authored;
        return result;
    }

    void prepareObservationMethods() {
        for (const auto& owner : program_.declarations) {
            for (const auto& method : owner.methods) {
                if (NativeNumericExports::admitsObservation(program_, owner, method))
                    observationMethods_.insert(method.identity);
            }
        }
    }

    void emitQualifiedDeclarations() {
        for (const auto& [identity, function] : qualifiedFunctions_) {
            (void)identity;
            line(cpp(function.method->result) + " " + function.name + "(" + qualifiedParameters(*function.method) + ");");
        }
    }

    void emitQualifiedDefinitions() {
        qualifiedEmission_ = true;
        usesBudget_ = true;
        objectEmission_ = false;
        workerEmission_ = false;
        classParallel_ = false;
        classSynchronization_ = false;
        for (const auto& [identity, function] : qualifiedFunctions_) {
            (void)identity;
            method_ = function.method;
            if (compiled_)
                beginImplementationDefinition(function.owner->name);
            line(std::string(compiled_ ? "" : "inline ") + cpp(method_->result) + " " + function.name +
                "(" + qualifiedParameters(*method_) + ") {");
            if (program_.objectEnabled) {
                line("auto pl_execution = pl_parentExecution.enterCall();", 1);
                line("pl_budget.step();", 1);
            }
            unusedParameters(*method_, 1);
            statements(method_->body, 1);
            line("}");
            if (compiled_) {
                endImplementationDefinition();
                selectedOutput_ = &output_;
            }
        }
        qualifiedEmission_ = false;
    }

    std::string numericReturnType(const Method& method, const NativeNumericExports::ArrayHelper* helper) const {
        if (method.result.name != "Array")
            return cpp(method.result);
        if (!helper || !helper->returnedArray)
            throw Diagnostic("numeric private array return lacks a fixed extent", method.offset);
        return "std::array<" + cpp(method.result.arguments.front()) + ", " +
            std::to_string(*helper->returnedArray) + ">";
    }

    std::string numericParameters(const Method& method, const NativeNumericExports::ArrayHelper* helper) const {
        std::string result;
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            const auto& parameter = method.parameters[index];
            if (!result.empty())
                result += ", ";
            if (parameter.type.name == "Array") {
                if (!helper || !helper->parameters.contains(index))
                    throw Diagnostic("numeric private array parameter lacks a fixed extent", parameter.offset);
                result += "std::array<" + cpp(parameter.type.arguments.front()) + ", " +
                    std::to_string(helper->parameters.at(index)) + ">";
            } else {
                result += cpp(parameter.type);
            }
            result += " " + parameter.name;
        }
        return result;
    }

    bool admitNumeric(const Declaration& owner, const Method& method, bool publicOnly = true,
                      NativeNumericExports::Projection* projection = nullptr) const {
        return standaloneHeader_
            ? NativeNumericExports::admitsHeader(program_, owner, method, publicOnly, projection)
            : NativeNumericExports::admits(program_, owner, method, publicOnly, projection);
    }

    void emitNumericMethod(const Method& method, const NativeNumericExports::Projection& projection,
                           const NativeNumericExports::ArrayHelper* helper = nullptr, bool privateOnly = false) {
        nativeProjection_ = projection;
        line(method.visible && !privateOnly ? "public:" : "private:");
        method_ = &method;
        line(std::string(projection.constantEvaluable ? "static constexpr " : "static ") +
            numericReturnType(method, helper) + " " + method.name +
            "(" + numericParameters(method, helper) + ") noexcept {", 1);
        unusedParameters(method, 2);
        statements(method.body, 2);
        line("}", 1);
    }

    void emitNativeNumericExports() {
        // Closed numeric values have a finite syntax/channel-bound execution cost.
        // Keep the ordinary scoped model API unchanged; only this additive
        // native projection omits ownership, graph and evaluation-budget state.
        usesBudget_ = false;
        objectEmission_ = false;
        workerEmission_ = false;
        numericEmission_ = true;
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                NativeNumericExports::Projection projection;
                if (!admitNumeric(declaration, method, true, &projection))
                    continue;
                if (projection.constantEvaluable)
                    numericConstantMethods_.insert(method.identity);
                const auto name = "pl_numeric_call_" + std::to_string(numericFunctions_.size());
                numericFunctions_.emplace(method.identity, name);
                line(std::string(projection.constantEvaluable ? "constexpr " : "") + cpp(method.result) +
                    " " + name + "(" + parameters(method) + ") noexcept;");
            }
        }
        for (const auto& declaration : program_.declarations) {
            struct MethodPlan {
                const Method* method;
                NativeNumericExports::Projection projection;
            };
            struct ArrayHelperPlan {
                const Method* method;
                NativeNumericExports::ArrayHelper signature;
                const NativeNumericExports::Projection* projection;
            };
            std::vector<MethodPlan> methods;
            bool selected = false;
            for (const auto& method : declaration.methods) {
                NativeNumericExports::Projection projection;
                if (!admitNumeric(declaration, method, false, &projection))
                    continue;
                selected = selected || method.visible;
                methods.push_back({&method, std::move(projection)});
            }
            if (!selected)
                continue;
            std::vector<ArrayHelperPlan> helpers;
            for (const auto& plan : methods) {
                for (const auto& [helper, signature] : plan.projection.arrayHelpers) {
                    const bool ownHelper = std::any_of(declaration.methods.begin(), declaration.methods.end(), [&](const Method& method) {
                        return &method == helper;
                    });
                    if (!ownHelper)
                        continue;
                    const bool duplicate = std::any_of(helpers.begin(), helpers.end(), [&](const ArrayHelperPlan& emitted) {
                        return emitted.method == helper && emitted.signature == signature;
                    });
                    if (!duplicate)
                        helpers.push_back({helper, signature, &plan.projection});
                }
            }
            auto [exportNamespace, typeName] = sourceAliasLocation(declaration.sourceName);
            exportNamespace.replace(0, std::string("source").size(), "native_numeric");
            line("namespace " + exportNamespace + " {");
            // Source methods may share the authored class name. A reserved
            // implementation name prevents C++ from interpreting them as
            // constructors while the alias preserves the source-qualified API.
            const auto implementationName = "pl_numeric_" + declaration.name;
            line("struct " + implementationName + " final {");
            for (const auto& plan : methods) {
                const bool arraySignature = plan.method->result.name == "Array" ||
                    std::any_of(plan.method->parameters.begin(), plan.method->parameters.end(), [](const Field& parameter) {
                        return parameter.type.name == "Array";
                    });
                if (!arraySignature)
                    emitNumericMethod(*plan.method, plan.projection);
            }
            for (const auto& helper : helpers)
                emitNumericMethod(*helper.method, *helper.projection, &helper.signature);
            line("};");
            line("using " + typeName + " = " + implementationName + ";");
            line("}");
        }
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                const auto found = numericFunctions_.find(method.identity);
                if (found == numericFunctions_.end())
                    continue;
                auto [exportNamespace, typeName] = sourceAliasLocation(declaration.sourceName);
                exportNamespace.replace(0, std::string("source").size(), "native_numeric");
                line(std::string(numericConstantMethods_.contains(method.identity) ? "inline constexpr " : "inline ") +
                    cpp(method.result) + " " + found->second + "(" + parameters(method) + ") noexcept {");
                line("return " + exportNamespace + "::" + typeName + "::" + method.name + "(" + argumentNames(method) + ");", 1);
                line("}");
            }
        }
    }

    void emitNativeObservationExports() {
        // Effectful observations use a separate API and manifest profile. The
        // admitted closure has no floating channels/arithmetic or owner state,
        // so it can inspect controls before any caller environment admission.
        usesBudget_ = false;
        objectEmission_ = false;
        workerEmission_ = false;
        numericEmission_ = true;
        observationEmission_ = true;
        for (const auto& owner : program_.declarations) {
            const bool selected = std::any_of(owner.methods.begin(), owner.methods.end(), [&](const Method& method) {
                return observationMethods_.contains(method.identity);
            });
            if (!selected)
                continue;
            auto [exportNamespace, typeName] = sourceAliasLocation(owner.sourceName);
            exportNamespace.replace(0, std::string("source").size(), "native_observation");
            line("namespace " + exportNamespace + " {");
            const auto implementationName = "pl_observation_" + owner.name;
            line("struct " + implementationName + " final {");
            for (const auto& method : owner.methods) {
                NativeNumericExports::Projection projection;
                if (!NativeNumericExports::admitsObservation(program_, owner, method, false, &projection, false))
                    continue;
                // Pure policy helpers and nonobserving effect helpers remain
                // private even when the authored method has public visibility.
                emitNumericMethod(method, projection, nullptr, !observationMethods_.contains(method.identity));
            }
            line("};");
            line("using " + typeName + " = " + implementationName + ";");
            line("}");
        }
        observationEmission_ = false;
        numericEmission_ = false;
    }

    void emitRootExecution(unsigned indentation, bool sourceCall = false,
                           const std::string& parentCalls = "pipelang::model::SourceCallContext{}",
                           bool alreadyEntered = false) {
        // The returned root owns its leases. Never borrow them from the
        // temporary execution produced while establishing a transaction.
        const auto callContext = sourceCall ? ".withCalls(" + parentCalls +
            (alreadyEntered ? "" : ".enter()") + ")" : "";
        line("[[maybe_unused]] auto pl_execution = pl_transaction.execution(pl_scope, pl_budget)" + callContext + ";", indentation);
        if (sourceCall)
            line("pl_budget.step();", indentation);
        if (classParallel_ || classSynchronization_)
            line("pl_execution.cancellation = pl_parallelCancellation;", indentation);
        if (classParallel_)
            line("pl_execution.maximumDegree = pl_parallelOptions.maximumDegreeOfParallelism;", indentation);
    }
    bool objectClass(const Declaration& declaration) const {
        return program_.objectEnabled && declaration.kind == "Class" && !declaration.modelExported;
    }
    bool objectReference(const Type& type) const {
        const auto* declaration = findType(program_, type.name);
        return declaration && (objectClass(*declaration) ||
            (program_.objectEnabled && declaration->sourceInterface));
    }
    bool referenceBearing(const Type& type) const {
        return objectReference(type) || std::any_of(type.arguments.begin(), type.arguments.end(),
            [&](const Type& argument) { return referenceBearing(argument); });
    }
    std::string objectTag(const std::string& owner) const {
        return "pl_object_tag_" + owner;
    }
    std::string objectState(const std::string& owner) const {
        return "pl_object_state_" + owner;
    }
    std::string objectSlot(const std::string& name) const {
        return "pl_field_" + name;
    }
    std::string objectStorage(const Type& type) const {
        return "pipelang::model::ObjectStorage<" + cpp(type) + ">";
    }
    std::string objectDomain() const {
        return "pl_execution.transaction.domain()";
    }
    std::string transactionType() const {
        return std::string(program_.objectEnabled ? "pipelang::model::GraphModelTransaction<pl_State>"
                                                 : "pipelang::model::Transaction<pl_State>");
    }
    const Method& objectMethod(const std::string& identity) const {
        return *objectMethods_.at(identity);
    }
    std::string objectFunction(const std::string& identity) const {
        return objectFunctions_.at(identity);
    }
    void prepareObjectNames() {
        std::size_t type = 1;
        std::size_t method = 0;
        for (const auto& declaration : program_.declarations) {
            if (objectClass(declaration))
                objectTypeIds_[declaration.name] = type++;
            if (declaration.kind != "Class" && !declaration.sourceInterface)
                continue;
            for (const auto& callable : declaration.methods) {
                if (!objectClass(declaration) && !declaration.sourceInterface && !callable.staticMethod)
                    continue;
                objectMethods_[callable.identity] = &callable;
                objectFunctions_[callable.identity] = "pl_object_function_" + std::to_string(method++);
            }
        }
    }
    void emitObjectTags() {
        if (!program_.objectEnabled)
            return;
        prepareObjectNames();
        for (const auto& declaration : program_.declarations) {
            if (!objectClass(declaration) && !declaration.sourceInterface)
                continue;
            line("struct " + objectTag(declaration.name) + " {");
            line("using PureObjectTagOwner = " + objectTag(declaration.name) + ";", 1);
            const auto id = objectClass(declaration) ? objectTypeIds_.at(declaration.name) : 0;
            line("static constexpr std::uint64_t typeId = " + std::to_string(id) + ";", 1);
            line("static bool accepts(std::uint64_t value) noexcept {", 1);
            line("(void)value;", 2);
            if (objectClass(declaration)) {
                std::string accepted = "value == typeId";
                for (const auto& candidate : program_.declarations) {
                    if (objectClass(candidate) && candidate.name != declaration.name && derivesFrom(candidate, declaration.name))
                        accepted += " || value == " + std::to_string(objectTypeIds_.at(candidate.name));
                }
                line("return " + accepted + ";", 2);
            } else {
                std::string expression = "false";
                for (const auto& candidate : program_.declarations) {
                    if (objectClass(candidate) && std::any_of(candidate.interfaces.begin(), candidate.interfaces.end(),
                        [&](const Type& type) { return type.name == declaration.name; }))
                        expression += " || value == " + std::to_string(objectTypeIds_.at(candidate.name));
                }
                line("return " + expression + ";", 2);
            }
            line("}", 1);
            line("};");
        }
    }
    bool derivesFrom(const Declaration& owner, const std::string& base) const {
        return owner.name == base || std::find(owner.ancestors.begin(), owner.ancestors.end(), base) != owner.ancestors.end();
    }
    const Method* effectiveObjectMethod(const Declaration& owner, const Method& slot) const {
        for (const auto& candidate : owner.methods) {
            if (candidate.constructor || candidate.name != slot.name || candidate.parameters.size() != slot.parameters.size())
                continue;
            bool matches = true;
            for (std::size_t index = 0; index < slot.parameters.size(); ++index)
                matches = matches && candidate.parameters[index].type == slot.parameters[index].type;
            if (matches)
                return &candidate;
        }
        return owner.baseClass.empty() ? nullptr : effectiveObjectMethod(*findType(program_, owner.baseClass), slot);
    }
    const Declaration& objectMethodOwner(const Method& method) const {
        return *findType(program_, method.identity.substr(0, method.identity.find('.')));
    }
    std::string objectCallFunction(const Expr& call) const {
        const bool dynamic = call->resolution == "class_virtual_call" || call->resolution == "class_virtual_async";
        return objectFunction(call->callable) + (dynamic ? "_dispatch" : "");
    }
    std::string objectExecutionType() const {
        return program_.projectEnabled && !compiled_ ? "pl_Execution" : "pipelang::model::ObjectExecution";
    }
    std::string objectTemplatePrefix() const {
        if (!program_.projectEnabled || compiled_)
            return {};
        // Keep every implementation and public dispatch route. Dependence on
        // this exact execution type defers unused private bodies and their
        // nested closures until a native or Source caller needs them.
        return "template<class pl_Execution>\n"
               "requires std::is_same_v<pl_Execution, pipelang::model::ObjectExecution>\n";
    }
    std::string objectTemplateKeyword() const {
        return lazyObjectBody_ ? "template " : "";
    }
    std::string objectInitializerParameters(const Declaration& owner, const Method& method) const {
        std::string result = objectExecutionType() + "& pl_parentExecution, " + cpp({owner.name, {}, owner.sourceName}) + " pl_self";
        for (const auto& parameter : method.parameters)
            result += ", " + cpp(parameter.type) + " " + parameter.name;
        return result;
    }
    std::string objectReturn(const Declaration& owner, const Method& method) const {
        if (method.constructor)
            return cpp({owner.name, {}, owner.sourceName});
        if (method.asynchronous)
            return "pipelang::model::Coroutine<" + cpp(method.result.arguments[0]) + ">";
        return cpp(method.result);
    }
    std::string objectParameters(const Declaration& owner, const Method& method, bool body = false) const {
        std::string result = objectExecutionType() + (method.asynchronous && body ? " " : "& ") +
            (body ? "pl_parentExecution" : "pl_execution");
        if (!method.constructor && !method.staticMethod)
            result += ", " + cpp({owner.name, {}, owner.sourceName}) + " pl_self";
        for (const auto& parameter : method.parameters)
            result += ", " + cpp(parameter.type) + " " + parameter.name;
        if (method.asynchronous)
            result += ", pipelang::model::CancellationToken pl_taskCancellation";
        return result;
    }
    std::string objectFieldReceiver(const Expr& field) {
        return field->kind == "name" ? "pl_self" : expression(field->children[0]);
    }
    std::string objectFieldValue(const Expr& field) {
        return objectStorage(field->type) + "::decode(pl_execution.transaction." + objectTemplateKeyword() + "read<" +
            objectState(field->owner) + ">(" + objectFieldReceiver(field) + ")." + objectSlot(field->text) +
            ", " + objectDomain() + ")";
    }
    Expr objectFieldRoot(const Expr& target) const {
        auto root = target;
        while (root && root->kind == "member" && root->resolution == "record")
            root = root->children[0];
        return root && root->resolution == "object_field" ? root : Expr{};
    }
    std::string objectFieldPath(const Expr& target, const Expr& root) const {
        if (target == root)
            return objectSlot(root->text);
        return objectFieldPath(target->children[0], root) + "." + target->text;
    }
    bool storedRecordProjection(const Expr& target) const {
        if (!target || target->kind != "member" || target->resolution != "record")
            return false;
        const auto field = objectFieldRoot(target);
        if (!field)
            return false;
        // Only owned record paths have the same stored and public layout.
        // Preserve the ordinary decoder for conversions or reference-bearing
        // storage. The checker independently requires owned record fields.
        for (auto part = target;; part = part->children[0]) {
            if (!part->convertTo.name.empty() || referenceBearing(part->type))
                return false;
            if (part == field)
                break;
        }
        const auto* declaration = findType(program_, field->type.name);
        return declaration && declaration->kind == "Record";
    }
    std::string objectRecordProjection(const Expr& target) {
        const auto field = objectFieldRoot(target);
        std::string result = "([&]() { ";
        // expression() already charged the selected member. Preserve every
        // receiver member/field tick before evaluating the object receiver.
        if (usesBudget_) {
            for (auto part = target->children[0];; part = part->children[0]) {
                result += "pl_budget.step(); ";
                if (part == field)
                    break;
            }
        }
        // Root temporary handles for the entire read. Copy only the selected
        // field into an owned value before any later argument or host effect;
        // neither a stored reference nor its lifetime escapes the lambda.
        result += "auto pl_objectReceiver = " + objectFieldReceiver(field) + "; ";
        result += "const auto& pl_selectedField = pl_execution.transaction." + objectTemplateKeyword() + "read<" +
            objectState(field->owner) + ">(pl_objectReceiver)." + objectFieldPath(target, field) + "; ";
        result += "return " + cpp(target->type) + "(pl_selectedField); ";
        return result + "}())";
    }
    void emitObjectAssignment(const Expr& target, const std::string& value, unsigned indentation) {
        const auto field = objectFieldRoot(target);
        const auto suffix = std::to_string(objectSequence_++);
        const auto receiver = "pl_objectReceiver" + suffix;
        const auto assigned = "pl_objectAssigned" + suffix;
        line("{", indentation);
        line("auto " + receiver + " = " + objectFieldReceiver(field) + ";", indentation + 1);
        line("auto " + assigned + " = " + value + ";", indentation + 1);
        line("validate(" + assigned + ");", indentation + 1);
        line("pl_execution.transaction." + objectTemplateKeyword() + "mutate<" + objectState(field->owner) + ">(" + receiver + ", [&](auto& pl_candidate) {", indentation + 1);
        if (target == field) {
            line("pl_candidate." + objectSlot(field->text) + " = " + objectStorage(field->type) + "::encode(" + assigned + ", " + objectDomain() + ");", indentation + 2);
        } else {
            line("pl_candidate." + objectFieldPath(target, field) + " = std::move(" + assigned + ");", indentation + 2);
        }
        line("});", indentation + 1);
        line("}", indentation);
    }
    std::string objectCollectionMutation(const Expr& value) {
        const auto& receiverExpression = value->children[0]->children[0];
        const auto field = objectFieldRoot(receiverExpression);
        std::string result = "([&]() { auto pl_objectReceiver = " + objectFieldReceiver(field) + "; ";
        std::string arguments;
        for (std::size_t index = 1; index < value->children.size(); ++index) {
            const auto name = "pl_objectArgument" + std::to_string(index);
            result += "auto " + name + " = " + expression(value->children[index]) + "; ";
            result += "validate(" + name + "); ";
            if (!arguments.empty())
                arguments += ", ";
            arguments += name;
        }
        const auto operation = value->resolution == "list_add" ? "add" : value->resolution.substr(18);
        if (value->type.name != "void")
            result += "std::optional<" + cpp(value->type) + "> pl_objectResult; ";
        result += "pl_execution.transaction." + objectTemplateKeyword() + "mutate<" + objectState(field->owner) + ">(pl_objectReceiver, [&](auto& pl_candidate) { ";
        // Materialize the field so stored edges do not escape into local values.
        result += "auto pl_collection = " + objectStorage(field->type) + "::decode(pl_candidate." + objectSlot(field->text) + ", " + objectDomain() + "); ";
        std::string path = "pl_collection";
        if (receiverExpression != field)
            path += objectFieldPath(receiverExpression, field).substr(objectSlot(field->text).size());
        const auto call = path + "." + operation + "(" + arguments + ")";
        if (value->type.name == "void")
            result += call + "; ";
        else
            result += "pl_objectResult.emplace(" + call + "); ";
        result += "pl_candidate." + objectSlot(field->text) + " = " + objectStorage(field->type) + "::encode(pl_collection, " + objectDomain() + "); }); ";
        if (value->type.name != "void")
            result += "return std::move(*pl_objectResult); ";
        return result + "}())";
    }
    bool storedSequenceReceiver(const Expr& receiver) const {
        return receiver && (receiver->type.name == "List" || receiver->type.name == "Array") &&
            objectFieldRoot(receiver) && receiver->convertTo.name.empty();
    }
    std::string objectSequenceRead(const Expr& receiver, const Expr& index = {}) {
        const auto field = objectFieldRoot(receiver);
        std::string result = "([&]() { ";
        // The original receiver evaluates each record member and field before
        // the object handle. Preserve those ticks without decoding the sequence.
        if (usesBudget_) {
            for (auto part = receiver;; part = part->children[0]) {
                result += "pl_budget.step(); ";
                if (part == field)
                    break;
            }
        }
        // Keep even a temporary receiver rooted until the selected element has
        // been decoded into an owned value. No stored reference escapes this call.
        result += "auto pl_objectReceiver = " + objectFieldReceiver(field) + "; ";
        result += "const auto& pl_storedSequence = pl_execution.transaction." + objectTemplateKeyword() + "read<" +
            objectState(field->owner) + ">(pl_objectReceiver)." + objectFieldPath(receiver, field) + "; ";
        if (index) {
            result += "auto pl_index = " + expression(index) + "; ";
            result += "return " + objectStorage(receiver->type) +
                "::storedAt(pl_storedSequence, pl_index, " + objectDomain() + "); ";
        } else {
            result += "return " + objectStorage(receiver->type) +
                "::storedCount(pl_storedSequence, " + objectDomain() + "); ";
        }
        return result + "}())";
    }
    std::string objectCall(const Expr& value) {
        std::string result = "([&]() { ";
        std::string arguments = "pl_execution";
        if (value->resolution != "object_construct" && value->resolution != "static_call") {
            result += "auto pl_objectReceiver = " + expression(value->children[0]->children[0]) + "; ";
            arguments += ", pl_objectReceiver." + objectTemplateKeyword() + "as<" + objectTag(value->owner) + ">()";
        }
        for (std::size_t index = 1; index < value->children.size(); ++index) {
            const auto name = "pl_objectArgument" + std::to_string(index);
            result += "auto " + name + " = " + expression(value->children[index]) + "; ";
            arguments += ", std::move(" + name + ")";
        }
        return result + "return " + objectCallFunction(value) + "(" + arguments + "); }())";
    }
    std::string statelessObjectWorker(const Expr& value) {
        if (!value->statelessWorkerCall || !objectMethod(value->callable).statelessWorkerCallable)
            throw Diagnostic("object worker call lacks stateless proof", value->offset);
        // Preserve the synthetic receiver evaluation's budget charge, without
        // carrying an ObjectRef, graph transaction or owner state to the worker.
        std::string result = "([&]() { pl_budget.step(); ";
        std::string arguments = "pl_budget, pl_calls";
        for (std::size_t index = 1; index < value->children.size(); ++index) {
            const auto name = "pl_workerArgument" + std::to_string(index);
            result += "auto " + name + " = " + expression(value->children[index]) + "; ";
            arguments += ", std::move(" + name + ")";
        }
        return result + "return " + objectFunction(value->callable) + "_value_worker(" + arguments + "); }())";
    }

    std::string statelessWorkerParameters(const Method& method) const {
        std::string result = "pipelang::model::EvaluationBudget& pl_budget, pipelang::model::SourceCallContext pl_parentCalls";
        for (const auto& parameter : method.parameters)
            result += ", " + cpp(parameter.type) + " " + parameter.name;
        return result;
    }

    void emitStatelessObjectWorkers() {
        for (const auto& declaration : program_.declarations) {
            if (!objectClass(declaration))
                continue;
            for (const auto& method : declaration.methods) {
                if (method.statelessWorkerCallable)
                    line(objectInline() + cpp(method.result) + " " + objectFunction(method.identity) +
                         "_value_worker(" + statelessWorkerParameters(method) + ");");
            }
        }
        // Definitions use the existing implementation owner when compiled
        // output is selected; no new translation-unit partition is introduced.
        for (const auto& declaration : program_.declarations) {
            if (!objectClass(declaration))
                continue;
            for (const auto& method : declaration.methods) {
                if (!method.statelessWorkerCallable)
                    continue;
                beginObjectImplementation(declaration);
                method_ = &method;
                workerEmission_ = true;
                workerCaptures_.clear();
                line(objectInline() + cpp(method.result) + " " + objectFunction(method.identity) +
                     "_value_worker(" + statelessWorkerParameters(method) + ") {");
                line("[[maybe_unused]] const auto pl_calls = pl_parentCalls.enter();", 1);
                line("pl_budget.step();", 1);
                validateParameters(method, 1);
                unusedParameters(method, 1);
                statements(method.body, 1);
                line("throw pipelang::model::ExecutionFailure(\"unreachable stateless worker control flow\");", 1);
                line("}");
                workerEmission_ = false;
                endObjectImplementation();
            }
        }
    }

    void emitObjectAwait(const Statement& statement, unsigned indentation) {
        const auto& call = statement.expression->children[0];
        const auto suffix = std::to_string(awaitSequence_++);
        const auto receiver = "pl_objectAwaitReceiver" + suffix;
        const auto cancellation = "pl_objectAwaitCancellation" + suffix;
        const auto operation = "pl_objectAwaitOperation" + suffix;
        const auto result = "pl_objectAwaitResult" + suffix;
        if (call->resolution != "static_async")
            line("auto " + receiver + " = " + expression(call->children[0]->children[0]) + ";", indentation);
        line("auto " + cancellation + " = pl_taskCancellation;", indentation);
        std::string arguments = "pl_execution";
        if (call->resolution != "static_async")
            arguments += ", " + receiver + "." + objectTemplateKeyword() + "as<" + objectTag(call->owner) + ">()";
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            const auto name = "pl_objectAwaitArgument" + suffix + "_" + std::to_string(index);
            line("auto " + name + " = " + expression(call->children[index]) + ";", indentation);
            arguments += ", std::move(" + name + ")";
        }
        arguments += ", " + cancellation;
        line("std::optional<" + cpp(statement.expression->type) + "> " + result + ";", indentation);
        line("pl_execution.commitSegment();", indentation);
        line("pl_execution.refreshSegment();", indentation);
        line("try {", indentation);
        line("auto " + operation + " = " + objectCallFunction(call) + "(" + arguments + ");", indentation + 1);
        line(result + ".emplace(co_await std::move(" + operation + "));", indentation + 1);
        line("} catch (...) {", indentation);
        line("pl_execution.refreshSegment();", indentation + 1);
        line("throw;", indentation + 1);
        line("}", indentation);
        line("pl_execution.refreshSegment();", indentation);
        storeAwaited(statement, "std::move(*" + result + ")", indentation);
    }
    void emitObjectDefinitions() {
        if (!program_.objectEnabled)
            return;
        usesBudget_ = true;
        lazyObjectBody_ = program_.projectEnabled && !compiled_;
        std::vector<const Declaration*> ordered;
        for (const auto& declaration : program_.declarations) {
            if (objectClass(declaration))
                ordered.push_back(&declaration);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const Declaration* left, const Declaration* right) {
            return left->ancestors.size() < right->ancestors.size();
        });
        for (const auto* current : ordered) {
            const auto& declaration = *current;
            std::vector<Field> fields;
            const auto addFields = [&](const Declaration& owner) {
                for (const auto& field : owner.fields)
                    fields.push_back(field);
            };
            addFields(declaration);
            for (const auto& ancestor : declaration.ancestors)
                addFields(*findType(program_, ancestor));
            if (!objectClass(declaration))
                continue;
            memberOwner_ = objectState(declaration.name);
            implementationOwner_ = declaration.name;
            line("struct " + objectState(declaration.name) + " : " + (declaration.baseClass.empty() ? "pipelang::model::ObjectState" : objectState(declaration.baseClass)) + " {");
            for (const auto& field : declaration.fields)
                line("typename " + objectStorage(field.type) + "::Stored " + objectSlot(field.name) + "{};", 1);
            beginMember("std::unique_ptr<pipelang::model::ObjectState>", "clone", "", " const" + std::string(compiled_ ? "" : " override"));
            line("return std::make_unique<" + objectState(declaration.name) + ">(*this);", 2);
            endMember();
            beginMember("void", "trace", "pipelang::model::ObjectTrace& pl_trace", " const" + std::string(compiled_ ? "" : " override"));
            line("(void)pl_trace;", 2);
            for (const auto& field : fields)
                line(objectStorage(field.type) + "::trace(" + objectSlot(field.name) + ", pl_trace);", 2);
            endMember();
            beginMember("std::size_t", "logicalBytes", "", " const" + std::string(compiled_ ? "" : " override"));
            line("using pipelang::model::ownedBytes;", 2);
            line("std::size_t pl_bytes = sizeof(*this);", 2);
            for (const auto& field : fields)
                line("pl_bytes = pipelang::model::addOwnedBytes(pl_bytes, ownedBytes(" + objectSlot(field.name) + "));", 2);
            line("return pl_bytes;", 2);
            endMember();
            beginMember("void", "validate", "", " const" + std::string(compiled_ ? "" : " override"));
            line("using pipelang::model::validate;", 2);
            for (const auto& field : fields) {
                if (field.type.name == "ContextOwner") {
                    line("if (" + objectSlot(field.name) + ")", 2);
                    line("validate(" + objectSlot(field.name) + ");", 3);
                } else if (!service(program_, field.type)) {
                    line("validate(" + objectSlot(field.name) + ");", 2);
                }
            }
            endMember();
            beginMember("void", "validateInitialized", "", " const" + std::string(compiled_ ? "" : " override"));
            line("validate();", 2);
            for (const auto& field : fields)
                line("pipelang::model::ObjectRequired<" + cpp(field.type) + ">::validate(" + objectSlot(field.name) + ");", 2);
            for (const auto& field : fields) {
                if (service(program_, field.type)) {
                    line("if (!" + objectSlot(field.name) + ")", 2);
                    line("throw pipelang::model::ExecutionFailure(\"constructor must initialize every host service\");", 3);
                }
            }
            endMember();
            line("};");
        }
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind != "Class" && !declaration.sourceInterface)
                continue;
            for (const auto& method : declaration.methods) {
                if (!objectClass(declaration) && !declaration.sourceInterface && !method.staticMethod)
                    continue;
                line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                     "(" + objectParameters(declaration, method) + ");");
                if (method.constructor)
                    line(objectTemplatePrefix() + objectInline() + "void " + objectFunction(method.identity) + "_initialize(" + objectInitializerParameters(declaration, method) + ");");
                if (method.virtualMethod)
                    line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                         "_dispatch(" + objectParameters(declaration, method) + ");");
                if ((objectClass(declaration) || method.staticMethod) && method.asynchronous)
                    line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                         "_body(" + objectParameters(declaration, method, true) + ");");
            }
        }
        for (const auto& declaration : program_.declarations) {
            if (!declaration.sourceInterface && !objectClass(declaration))
                continue;
            for (const auto& method : declaration.methods) {
                if (!declaration.sourceInterface && !method.virtualMethod)
                    continue;
                beginObjectImplementation(declaration);
                line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                     (declaration.sourceInterface ? "" : "_dispatch") + "(" + objectParameters(declaration, method) + ") {");
                line("(void)pl_execution;", 1);
                for (const auto& parameter : method.parameters)
                    line("(void)" + parameter.name + ";", 1);
                if (method.asynchronous)
                    line("(void)pl_taskCancellation;", 1);
                line("pl_self.require();", 1);
                line("switch (pl_self.ownerDomain()->runtimeType(pl_self.identity())) {", 1);
                for (const auto& concrete : program_.declarations) {
                    const bool compatible = declaration.sourceInterface
                        ? std::any_of(concrete.interfaces.begin(), concrete.interfaces.end(), [&](const Type& type) { return type.name == declaration.name; })
                        : derivesFrom(concrete, declaration.name);
                    if (!objectClass(concrete) || !compatible)
                        continue;
                    const auto* found = effectiveObjectMethod(concrete, method);
                    const auto& declaring = objectMethodOwner(*found);
                    std::string arguments = "pl_execution, pl_self." + objectTemplateKeyword() + "as<" + objectTag(declaring.name) + ">()";
                    for (const auto& parameter : method.parameters)
                        arguments += ", std::move(" + parameter.name + ")";
                    if (method.asynchronous)
                        arguments += ", pl_taskCancellation";
                    line("case " + std::to_string(objectTypeIds_.at(concrete.name)) + ":", 1);
                    line("return " + objectFunction(found->identity) + "(" + arguments + ");", 2);
                }
                line("default:", 1);
                line("throw pipelang::model::ExecutionFailure(\"incompatible source interface receiver\");", 2);
                line("}", 1);
                line("}");
                endObjectImplementation();
            }
        }
        emitStatelessObjectWorkers();
        objectEmission_ = true;
        classSynchronization_ = true;
        classParallel_ = usesParallel();
        localAsync_ = true;
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind != "Class")
                continue;
            objectOwner_ = &declaration;
            for (const auto& method : declaration.methods) {
                if (!objectClass(declaration) && !method.staticMethod)
                    continue;
                beginObjectImplementation(declaration);
                method_ = &method;
                internalEffect_ = !method.asynchronous;
                if (method.asynchronous) {
                    // This ordinary factory checks ancestry before a child
                    // coroutine frame can be allocated, including native Source
                    // callers. Only the body below contains co_await/co_return.
                    line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                         "(" + objectParameters(declaration, method) + ") {");
                    line("auto pl_childExecution = pl_execution.enterAsyncCall();", 1);
                    std::string arguments = "std::move(pl_childExecution)";
                    if (!method.staticMethod)
                        arguments += ", std::move(pl_self)";
                    for (const auto& parameter : method.parameters)
                        arguments += ", std::move(" + parameter.name + ")";
                    arguments += ", pl_taskCancellation";
                    line("return " + objectFunction(method.identity) + "_body(" + arguments + ");", 1);
                    line("}");
                    endObjectImplementation();
                    beginObjectImplementation(declaration);
                }
                if (method.constructor) {
                    line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                         "(" + objectParameters(declaration, method) + ") {");
                    // Refuse depth before allocating the constructor's cell.
                    // The initializer owns the one actual source activation,
                    // including direct base-initializer calls.
                    line("(void)pl_execution.calls().enter();", 1);
                    line("auto pl_construction = pl_execution.transaction.savepoint();", 1);
                    line("auto pl_self = pl_execution.transaction." + objectTemplateKeyword() + "create<" + objectState(declaration.name) + ">(" + objectTag(declaration.name) + "{});", 1);
                    std::string arguments = "pl_execution, pl_self";
                    for (const auto& parameter : method.parameters)
                        arguments += ", std::move(" + parameter.name + ")";
                    line(objectFunction(method.identity) + "_initialize(" + arguments + ");", 1);
                    line("pl_execution.transaction.validateInitialized(pl_self);", 1);
                    line("pl_construction.commit();", 1);
                    line("return pl_self;", 1);
                    line("}");
                    endObjectImplementation();
                    beginObjectImplementation(declaration);
                    line(objectTemplatePrefix() + objectInline() + "void " + objectFunction(method.identity) + "_initialize(" + objectInitializerParameters(declaration, method) + ") {");
                } else {
                    line(objectTemplatePrefix() + objectInline() + objectReturn(declaration, method) + " " + objectFunction(method.identity) +
                         (method.asynchronous ? "_body" : "") + "(" + objectParameters(declaration, method, true) + ") {");
                }
                line(std::string("auto pl_execution = ") +
                     (method.asynchronous ? "std::move(pl_parentExecution);" : "pl_parentExecution.enterCall();"), 1);
                line("auto& pl_budget = pl_execution.budget;", 1);
                line("pl_budget.step();", 1);
                line("[[maybe_unused]] auto pl_parallelCancellation = " +
                     std::string(method.asynchronous ? "pl_taskCancellation" : "pl_execution.cancellationToken()") + ";", 1);
                if (classParallel_)
                    line("[[maybe_unused]] pipelang::model::ParallelOptions pl_parallelOptions{pl_execution.maximumDegree};", 1);
                validateParameters(method, 1);
                unusedParameters(method, 1);
                if (method.constructor) {
                    line("pl_self.require();", 1);
                    if (method.baseConstructor) {
                        std::string arguments = "pl_execution, pl_self." + objectTemplateKeyword() + "as<" + objectTag(declaration.baseClass) + ">()";
                        for (std::size_t index = 1; index < method.baseConstructor->children.size(); ++index) {
                            const auto temporary = "pl_baseArgument" + std::to_string(index);
                            line("auto " + temporary + " = " + expression(method.baseConstructor->children[index]) + ";", 1);
                            arguments += ", std::move(" + temporary + ")";
                        }
                        line(objectFunction(method.baseConstructor->callable) + "_initialize(" + arguments + ");", 1);
                    }
                    for (std::size_t index = 0; index < declaration.fields.size(); ++index) {
                        const auto& initializer = method.fieldInitializers[index];
                        if (!initializer)
                            continue;
                        auto target = std::make_shared<Expression>();
                        target->kind = "name";
                        target->text = declaration.fields[index].name;
                        target->type = declaration.fields[index].type;
                        target->owner = declaration.name;
                        target->resolution = "object_field";
                        emitObjectAssignment(target, expression(initializer), 1);
                    }
                } else if (!method.staticMethod) {
                    line("pl_self.require();", 1);
                }
                statements(method.body, 1);
                if (!method.constructor && method.result.name != "void") {
                    line("throw pipelang::model::ExecutionFailure(\"unreachable object control flow\");", 1);
                }
                line("}");
                endObjectImplementation();
            }
        }
        lazyObjectBody_ = false;
        objectEmission_ = false;
        objectOwner_ = nullptr;
        internalEffect_ = false;
    }

    bool usesType(const std::string& name) const {
        const auto found = typeUsage_.find(name);
        if (found != typeUsage_.end())
            return found->second;
        // This query observes only the finalized Program, not emission state.
        // Keep both positive and negative answers within this Generator lifetime.
        const auto used = programUsesType(program_, name);
        typeUsage_.emplace(name, used);
        return used;
    }
    bool usesSynchronousHosts() const {
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind != "Interface" || declaration.sourceInterface)
                continue;
            for (const auto& method : declaration.methods) {
                if (!method.asynchronous)
                    return true;
            }
        }
        return false;
    }
    bool usesSynchronization() const {
        return usesType("Lock") || usesType("SemaphoreSlim");
    }
    bool usesConcurrentCollections() const {
        return usesType("ConcurrentDictionary") || usesType("ConcurrentQueue") ||
            usesType("ConcurrentStack");
    }
    bool usesExtendedCollections() const {
        for (const auto* name : {"Array", "Dictionary", "HashSet", "Queue", "Stack", "KeyValuePair"}) {
            if (usesType(name))
                return true;
        }
        return false;
    }
    bool usesParallel() const {
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                if (containsStatement(method.body, "parallel_foreach"))
                    return true;
            }
        }
        return false;
    }
    bool usesLocalAsync(const std::vector<Statement>& body) const {
        std::function<bool(const Expr&)> inExpression = [&](const Expr& value) {
            return value && (value->resolution == "local_async" ||
                std::any_of(value->children.begin(), value->children.end(), inExpression));
        };
        return std::any_of(body.begin(), body.end(), [&](const Statement& statement) {
            return inExpression(statement.expression) || usesLocalAsync(statement.body) ||
                usesLocalAsync(statement.alternative) || usesLocalAsync(statement.cleanup) ||
                usesLocalAsync(statement.initialization) || usesLocalAsync(statement.iteration);
        });
    }
    bool usesLocalAsync() const {
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                if (usesLocalAsync(method.body))
                    return true;
            }
        }
        return false;
    }
    bool usesLoops(const std::vector<Statement>& body) const {
        return std::any_of(body.begin(), body.end(), [&](const Statement& statement) {
            return statement.kind == "while" || statement.kind == "foreach" || statement.kind == "for" ||
                usesLoops(statement.body) || usesLoops(statement.alternative) || usesLoops(statement.cleanup) ||
                usesLoops(statement.initialization) || usesLoops(statement.iteration);
        });
    }
    bool usesLoops() const {
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                if (usesLoops(method.body))
                    return true;
            }
        }
        return false;
    }
    bool containsStatement(const std::vector<Statement>& body, const std::string& kind) const {
        return std::any_of(body.begin(), body.end(), [&](const Statement& statement) {
            return statement.kind == kind || containsStatement(statement.body, kind) ||
                containsStatement(statement.alternative, kind) || containsStatement(statement.cleanup, kind) ||
                containsStatement(statement.initialization, kind) || containsStatement(statement.iteration, kind);
        });
    }
    bool hasFinally(const std::vector<Statement>& body) const {
        return std::any_of(body.begin(), body.end(), [&](const Statement& statement) {
            return statement.hasFinally || hasFinally(statement.body) ||
                hasFinally(statement.alternative) || hasFinally(statement.cleanup) ||
                hasFinally(statement.initialization) || hasFinally(statement.iteration);
        });
    }
    bool usesFailureControl() const {
        for (const auto& declaration : program_.declarations) {
            for (const auto& method : declaration.methods) {
                if (containsStatement(method.body, "try") || containsStatement(method.body, "throw"))
                    return true;
            }
        }
        return false;
    }
    std::string parallelParameters() const {
        if (workerEmission_)
            return "";
        if (classParallel_)
            return ", const pipelang::model::ParallelOptions& pl_parallelOptions, pipelang::model::CancellationToken pl_parallelCancellation";
        if (classSynchronization_)
            return ", pipelang::model::CancellationToken pl_parallelCancellation";
        return "";
    }
    std::string budgetArgument() const {
        std::string result = usesBudget_ ? ", pl_budget" : "";
        if (!workerEmission_) {
            if (classParallel_)
                result += ", pl_parallelOptions, pl_parallelCancellation";
            else if (classSynchronization_)
                result += ", pl_parallelCancellation";
        }
        if (program_.objectEnabled && !workerEmission_)
            result += ", pl_execution";
        if (program_.objectEnabled && workerEmission_)
            result += ", pl_calls";
        return result;
    }
    void cancellationContext(const Method& method, unsigned indentation, bool asynchronous = false) {
        if (!classParallel_ && !classSynchronization_)
            return;
        std::string cancellation = "pipelang::model::CancellationToken{}";
        if (asynchronous)
            cancellation = "pl_taskCancellation";
        else if (program_.objectEnabled)
            cancellation = "pipelang::model::currentPermit().cancellation()";
        line("auto pl_parallelCancellation = " + cancellation + ";", indentation);
        for (const auto& parameter : method.parameters) {
            if (!program_.objectEnabled && parameter.type.name == "CancellationToken")
                line("pl_parallelCancellation = pipelang::model::CancellationToken::combine(pl_parallelCancellation, " + parameter.name + ");", indentation);
        }
    }
    void unusedParameters(const Method& method, unsigned indentation) {
        std::set<std::string> used;
        std::function<void(const Expr&)> inspectExpression = [&](const Expr& expression) {
            if (!expression)
                return;
            if (expression->kind == "name" && expression->resolution == "local")
                used.insert(expression->text);
            for (const auto& child : expression->children)
                inspectExpression(child);
        };
        std::function<void(const std::vector<Statement>&)> inspectStatements = [&](const std::vector<Statement>& body) {
            for (const auto& statement : body) {
                inspectExpression(statement.expression);
                inspectExpression(statement.target);
                inspectStatements(statement.initialization);
                inspectStatements(statement.body);
                inspectStatements(statement.iteration);
                inspectStatements(statement.alternative);
                inspectStatements(statement.cleanup);
            }
        };
        inspectStatements(method.body);
        for (const auto& initializer : method.fieldInitializers)
            inspectExpression(initializer);
        for (const auto& parameter : method.parameters) {
            if (!used.contains(parameter.name))
                line("(void)" + parameter.name + ";", indentation);
        }
        if (usesBudget_ && method.body.empty())
            line("(void)pl_budget;", indentation);
    }
    void ignoreParallelParameters(unsigned indentation) {
        if (program_.objectEnabled)
            line("using pipelang::model::validate;", indentation);
        if (classParallel_)
            line("(void)pl_parallelOptions;", indentation);
        if (classParallel_ || classSynchronization_)
            line("(void)pl_parallelCancellation;", indentation);
        if (program_.objectEnabled && !workerEmission_)
            line("(void)pl_execution;", indentation);
    }
    void declareBudget(unsigned indentation, bool initialization = false) {
        if (!usesBudget_)
            return;
        if (initialization)
            line("auto pl_budget = pipelang::model::EvaluationBudget::forInitialization(pl_options.initializationSteps);", indentation);
        else
            line("auto pl_budget = pipelang::model::EvaluationBudget::forInvocation(pl_scope.evaluationStepsPerSegment());", indentation);
    }
    // A neutral operand cannot mutate state, fail, or change floating status.
    // Keeping every other operand ordered preserves Source evaluation effects.
    bool evaluationOrderNeutral(const Expr& value) const {
        if (!value->convertTo.name.empty())
            return false;
        if (value->resolution == "constant" || value->resolution == "enum" ||
            value->resolution == "ieee_constant" || value->resolution == "minimum_int")
            return true;
        if (value->kind == "integer" || value->kind == "uint32" || value->kind == "float" ||
            value->kind == "float32" || value->kind == "boolean")
            return true;
        if (value->kind == "name")
            return value->resolution == "local";
        return value->kind == "member" && value->resolution == "record" &&
            evaluationOrderNeutral(value->children.front());
    }

    bool directCallOrder(const Expr& value) const {
        bool hasEffectfulArgument = false;
        for (std::size_t index = 1; index < value->children.size(); ++index) {
            if (evaluationOrderNeutral(value->children[index]))
                continue;
            if (hasEffectfulArgument)
                return false;
            hasEffectfulArgument = true;
        }
        return true;
    }

    std::string floatingMathCall(const std::string& name, const std::string& arguments) const {
        const std::map<std::string, std::string> functions = {
            {"IsFinite", "isfinite"}, {"Abs", "abs"}, {"Sqrt", "sqrt"}, {"Log", "log"},
            {"Sin", "sin"}, {"Cos", "cos"}, {"Tan", "tan"}, {"Asin", "asin"},
            {"Acos", "acos"}, {"Atan", "atan"}, {"Atan2", "atan2"}, {"Exp", "exp"},
            {"Expm1", "expm1"}, {"Pow", "pow"}, {"Hypot", "hypot"}, {"Fmod", "fmod"},
            {"Remainder", "remainder"}, {"CopySign", "copysign"}, {"Cbrt", "cbrt"},
            {"Tanh", "tanh"}, {"Floor", "floor"}, {"Ceiling", "ceil"}
        };
        std::string call;
        if (name == "Clamp") {
            call = std::string(standaloneHeader_ ? "std::clamp(" : "pipelang::model::clampFinite(") + arguments + ")";
        } else if (name == "ILogB") {
            call = "pipelang::model::integerBinaryExponent(" + arguments + ")";
        } else if (name == "ScaleB") {
            call = "pipelang::model::scaleBinaryExponent(" + arguments + ")";
        } else {
            call = "std::" + functions.at(name) + "(" + arguments + ")";
        }
        return call;
    }

    std::optional<std::string> directNumericCall(const Expr& value) {
        if (!standaloneHeader_ || !directCallOrder(value))
            return std::nullopt;
        const auto& resolution = value->resolution;
        if (resolution != "numeric_conversion" && resolution != "numeric_round" &&
            resolution != "float_math" && resolution != "float_bits" &&
            resolution != "pure" && resolution != "qualified_pure" && resolution != "result_construct")
            return std::nullopt;
        std::string arguments;
        for (std::size_t index = 1; index < value->children.size(); ++index) {
            if (!arguments.empty())
                arguments += ", ";
            arguments += expression(value->children[index]);
        }
        const auto& target = value->children.front();
        if (resolution == "numeric_conversion")
            return "static_cast<" + cpp(value->type) + ">(" + arguments + ")";
        if (resolution == "numeric_round")
            return "static_cast<std::int64_t>(std::llround(" + arguments + "))";
        if (resolution == "float_math")
            return floatingMathCall(target->text, arguments);
        if (resolution == "float_bits")
            return "std::bit_cast<" + cpp(value->type) + ">(" + arguments + ")";
        if (resolution == "result_construct")
            return cpp(value->type) + "::" + target->text + "(" + arguments + ")";
        if (resolution == "qualified_pure")
            return numericFunctions_.at(value->callable) + "(" + arguments + ")";
        return target->text + "(" + arguments + ")";
    }

    std::string defaultExpression(const Expr& initializer) {
        auto generated = expression(initializer);
        // Independent record defaults have no receiver or local captures.
        std::size_t offset = 0;
        while ((offset = generated.find("[&]", offset)) != std::string::npos) {
            generated.replace(offset, 3, "[]");
            offset += 2;
        }
        return generated;
    }
    std::string cpp(const Type& type) const {
        if (numericEmission_ && type.name == "string")
            return "std::string_view";
        return cppType(type, program_);
    }
    void line(const std::string& text, unsigned indentation = 0) {
        *selectedOutput_ << std::string(indentation * 4, ' ') << text << '\n';
    }
    void beginMember(const std::string& result, const std::string& name,
                     const std::string& parameters, const std::string& suffix = {},
                     const std::string& prefix = {}, const std::string& initializer = {},
                     const std::optional<std::string>& definitionParameters = {},
                     const std::string& definitionResult = {}) {
        const auto signature = (result.empty() ? "" : result + " ") + name +
            "(" + parameters + ")" + suffix;
        if (!compiled_) {
            line(prefix + signature + initializer + " {", 1);
            return;
        }
        line(prefix + signature + ";", 1);
        memberHeader_ = selectedOutput_;
        beginImplementationDefinition(implementationOwner_);
        const auto returnType = definitionResult.empty() ? result : definitionResult;
        line((returnType.empty() ? "" : returnType + " ") + memberOwner_ + "::" + name +
            "(" + definitionParameters.value_or(parameters) + ")" + suffix + initializer + " {");
    }

    void endMember() {
        line("}", compiled_ ? 0 : 1);
        if (compiled_) {
            endImplementationDefinition();
            selectedOutput_ = memberHeader_;
        }
    }

    std::string objectInline() const {
        return compiled_ ? "" : "inline ";
    }

    void beginImplementationDefinition(const std::string& owner) {
        if (implementationDefinitionActive_)
            throw Diagnostic("nested compiled implementation definition", 0);
        implementationDefinition_.str({});
        implementationDefinition_.clear();
        implementationDefinitionOwner_ = owner;
        implementationDefinitionActive_ = true;
        selectedOutput_ = &implementationDefinition_;
    }

    void endImplementationDefinition() {
        if (!implementationDefinitionActive_)
            throw Diagnostic("missing compiled implementation definition", 0);
        implementationUnits_->append(implementationDefinitionOwner_, implementationDefinition_.str());
        implementationDefinitionActive_ = false;
    }

    void beginObjectImplementation(const Declaration& declaration) {
        if (compiled_)
            beginImplementationDefinition(declaration.name);
    }

    void endObjectImplementation() {
        if (compiled_) {
            endImplementationDefinition();
            selectedOutput_ = &objects_;
        }
    }

    std::string commitStatement() const {
        return objectEmission_ ? "pl_execution.commitSegment();" : "pl_transaction.commit();";
    }
    std::string refreshStatement() const {
        return objectEmission_ ? "pl_execution.refreshSegment();" : "pl_transaction.refresh();";
    }
    std::string scopeExpression() const {
        return objectEmission_ || qualifiedEmission_ ? "pl_execution.scope" : "pl_scope";
    }
    std::string parameters(const Method& method, bool state = false, bool cancellation = false) const {
        std::string result = state ? "const pl_State& pl_state" : "";
        if (state && usesBudget_)
            result += ", pipelang::model::EvaluationBudget& pl_budget";
        if (state) {
            result += parallelParameters();
            if (program_.objectEnabled && !workerEmission_)
                result += ", pipelang::model::ObjectExecution& pl_parentExecution";
        }
        for (const auto& parameter : method.parameters) {
            if (!result.empty())
                result += ", ";
            result += cpp(parameter.type) + " " + parameter.name;
        }
        if (cancellation) {
            if (!result.empty())
                result += ", ";
            result += "pipelang::model::CancellationToken pl_taskCancellation";
        }
        return result;
    }
    std::string argumentNames(const Method& method) const {
        std::string result;
        for (const auto& parameter : method.parameters) {
            if (!result.empty())
                result += ", ";
            result += parameter.name;
        }
        return result;
    }
    std::string state() const {
        if (workerEmission_)
            return "pl_workerState";
        if (objectEmission_)
            return "pl_execution.transaction." + objectTemplateKeyword() + "read<" + objectState(objectOwner_->name) + ">(pl_self)";
        return method_->pure ? "pl_state" : "pl_transaction.state()";
    }
    bool stableOwnedListReceiver(const Expr& value) const {
        if (!value || !value->convertTo.name.empty())
            return false;
        if (value->kind == "name" || value->kind == "model_member")
            return value->resolution == "local" || value->resolution == "field";
        if (value->kind == "member" && value->resolution == "record" && value->children.size() == 1)
            return stableOwnedListReceiver(value->children[0]);
        // Decoded object fields and Optional/Result.Value may denote temporary
        // owners. Keep an owned receiver snapshot rather than a dangling view.
        return false;
    }
    bool readOnlyListIndex(const Expr& value) const {
        if (!value)
            return false;
        const auto& kind = value->kind;
        if (kind != "integer" && kind != "float" && kind != "float32" && kind != "text" &&
            kind != "boolean" && kind != "name" && kind != "model_member" && kind != "this" && kind != "base" &&
            kind != "member" && kind != "unary" && kind != "binary" && kind != "conditional")
            return false;
        // Calls (including pure helpers), constructors, awaits and future
        // expression kinds conservatively retain the receiver's owned snapshot.
        return std::all_of(value->children.begin(), value->children.end(), [&](const Expr& child) {
            return readOnlyListIndex(child);
        });
    }
    std::string convertedExpression(std::string emitted, const Type& actual, const Type& expected) {
        if (actual == expected)
            return emitted;
        if (expected.name == "IResult" && actual.name == "Result")
            return cpp(expected) + "::From(" + emitted + ")";
        if (expected.name == "Optional") {
            if (actual.name == "$null")
                return "((void)(" + emitted + "), " + cpp(expected) + "::None())";
            return cpp(expected) + "::Some(" +
                convertedExpression(std::move(emitted), actual, expected.arguments[0]) + ")";
        }
        return "(" + emitted + ").template as<" + objectTag(expected.name) + ">()";
    }
    std::string expression(const Expr& value) {
        if (const auto found = propagationValues_.find(value.get()); found != propagationValues_.end())
            return found->second;
        auto emitted = expressionValue(value);
        if (!value->convertTo.name.empty())
            emitted = convertedExpression(std::move(emitted), value->type, value->convertTo);
        return usesBudget_ ? "(pl_budget.step(), " + emitted + ")" : emitted;
    }
    std::string expressionValue(const Expr& value) {
        if (value->kind == "propagate")
            throw Diagnostic("Result propagation must be lowered in its owning callable", value->offset);
        if (value->resolution == "cancellation_none")
            return "pipelang::model::CancellationToken{}";
        if (value->resolution == "floating_environment_observe")
            return "pipelang::model::observeFloatingEnvironment()";
        if (numericEmission_ && value->type.name == "FixedArray" &&
            value->resolution == "collection_construct") {
            return "([&]() { auto pl_initial = " + expression(value->children.at(1)) +
                "; return " + cpp(value->type) + "::filledUnchecked(pl_initial); }())";
        }
        if (numericEmission_ && value->type.name == "Array") {
            const auto proof = nativeProjection_.arrayValues.find(value.get());
            if (proof == nativeProjection_.arrayValues.end())
                throw Diagnostic("numeric array lacks a bounded local proof", value->offset);
            if (value->kind == "name")
                return value->text;
            if (value->resolution == "collection_construct") {
                const auto element = cpp(value->type.arguments.front());
                return "([&]() { auto pl_size = " + expression(value->children.at(1)) +
                    "; (void)pl_size; auto pl_initial = " + expression(value->children.at(2)) +
                    "; std::array<" + element + ", " + std::to_string(proof->second) +
                    "> pl_array; pl_array.fill(pl_initial); return pl_array; }())";
            }
        }
        if (numericEmission_ && value->kind == "member" && value->resolution == "list") {
            const auto& receiver = value->children.at(0);
            if (receiver->type.name == "FixedArray")
                return "([&]() { auto pl_array = " + expression(receiver) +
                    "; return pl_array.count(); }())";
            const auto proof = nativeProjection_.arrayValues.find(receiver.get());
            if (proof == nativeProjection_.arrayValues.end())
                throw Diagnostic("numeric array count lacks a bounded local proof", value->offset);
            return "([&]() { auto pl_array = " + expression(receiver) +
                "; return static_cast<std::int64_t>(pl_array.size()); }())";
        }
        if (numericEmission_ && nativeProjection_.arrayOperations.contains(value.get())) {
            const auto& receiver = value->children.at(0)->children.at(0);
            const bool mutating = value->resolution == "collection_mutate_set";
            std::string result = std::string("([&]() { auto") + (mutating ? "&" : "") +
                " pl_array = " + expression(receiver) + "; auto pl_index = " + expression(value->children.at(1)) + "; ";
            const std::string element = receiver->type.name == "FixedArray"
                ? "pl_array.uncheckedAt(static_cast<std::size_t>(pl_index))"
                : "pl_array[static_cast<std::size_t>(pl_index)]";
            if (mutating) {
                result += "auto pl_value = " + expression(value->children.at(2)) +
                    "; " + element + " = pl_value; ";
            } else {
                result += "return " + element + "; ";
            }
            return result + "}())";
        }
        if (value->kind == "null")
            return "std::nullopt";
        if (value->resolution == "constant")
            return constantLiteral(value->constantValue);
        if (value->resolution == "ieee_constant") {
            static const std::map<std::string, std::string> binary32{
                {"PositiveInfinity", "2139095040"}, {"NegativeInfinity", "4286578688"}, {"NaN", "2143289344"}
            };
            static const std::map<std::string, std::string> binary64{
                {"PositiveInfinity", "9218868437227405312"}, {"NegativeInfinity", "18442240474082181120"},
                {"NaN", "9221120237041090560"}
            };
            if (value->type.name == "float32")
                return "std::bit_cast<float>(std::uint32_t{" + binary32.at(value->text) + "U})";
            return "std::bit_cast<double>(std::uint64_t{" + binary64.at(value->text) + "ULL})";
        }
        if (value->kind == "this")
            return "pl_self";
        if (value->kind == "base")
            return "pl_self." + objectTemplateKeyword() + "as<" + objectTag(value->owner) + ">()";
        if (value->kind == "member" && value->resolution == "list" &&
            storedSequenceReceiver(value->children[0]))
            return objectSequenceRead(value->children[0]);
        if (value->kind == "call" && value->resolution == "list_at" && value->children.size() == 2 &&
            storedSequenceReceiver(value->children[0]->children[0]) && readOnlyListIndex(value->children[1]))
            return objectSequenceRead(value->children[0]->children[0], value->children[1]);
        if (storedRecordProjection(value))
            return objectRecordProjection(value);
        if (value->resolution == "object_field")
            return objectFieldValue(value);
        if (workerEmission_ && value->statelessWorkerCall)
            return statelessObjectWorker(value);
        if (value->resolution == "object_construct" || value->resolution == "object_call" || value->resolution == "static_call" || value->resolution == "source_interface_call" || value->resolution == "class_virtual_call")
            return objectCall(value);
        if ((value->resolution == "list_add" || value->resolution.starts_with("collection_mutate_")) &&
            value->kind == "call" && objectFieldRoot(value->children[0]->children[0]))
            return objectCollectionMutation(value);
        if (value->kind == "float32")
            return "std::bit_cast<float>(std::uint32_t{" + value->resolution + "U})";
        if (value->kind == "float")
            return "std::bit_cast<double>(std::uint64_t{" + value->resolution + "ULL})";
        if (value->kind == "uint32")
            return "std::uint32_t{" + value->resolution + "ULL}";
        if (value->kind == "integer")

            return "std::int64_t{" + value->text + "}";
        if (value->kind == "text")
            return escaped(value->text);
        if (value->kind == "boolean")
            return value->text;
        if (value->kind == "name" || value->kind == "model_member") {
            if (workerEmission_ && value->resolution == "local") {
                if (const auto capture = workerCaptures_.find(value->text); capture != workerCaptures_.end())
                    return capture->second;
            }
            if (value->resolution == "field")
                return state() + "." + value->text;
            if (value->resolution == "service")
                return "pl_service_" + value->text;
            return value->text;
        }
        if (value->kind == "unary") {
            if (value->resolution == "minimum_int")
                return "std::numeric_limits<std::int64_t>::min()";
            if (value->text == "~")
                return "pipelang::model::bitNot(" + expression(value->children[0]) + ")";
            if (value->text == "!")

                return "(!" + expression(value->children[0]) + ")";
            if (value->type.name == "float" || value->type.name == "float32")
                return "(-" + expression(value->children[0]) + ")";
            return "pipelang::model::negate(" + expression(value->children[0]) + ")";
        }
        if (value->kind == "member") {
            if (value->resolution == "enum")
                return value->type.name + "::" + value->text;
            const auto receiver = expression(value->children[0]);
            if (value->resolution == "pair_key" || value->resolution == "pair_value")
                return "(" + receiver + ")." + (value->resolution == "pair_key" ? "key" : "value");
            if (value->resolution == "dictionary_keys" || value->resolution == "dictionary_values" || value->resolution == "dictionary_entries") {
                const std::map<std::string, std::string> functions = {
                    {"dictionary_keys", "keys"}, {"dictionary_values", "mappedValues"}, {"dictionary_entries", "entries"}
                };
                return "(" + receiver + ")." + functions.at(value->resolution) + "()";
            }
            if (value->resolution == "sync_count")
                return "(" + receiver + ").currentCount()";
            if (value->resolution == "error")
                return "(" + receiver + ").message";
            if (value->resolution == "bytebuffer_length")
                return "(" + receiver + ").length()";
            if (value->resolution == "list")
                return "(" + receiver + ").count()";
            if (value->resolution == "optional")
                return "(" + receiver + ")." + (value->text == "HasValue" ? "has_value" : "value") + "()";
            if (value->resolution == "result") {
                const std::map<std::string, std::string> names = {
                    {"IsSuccess", "is_success"}, {"IsFailure", "is_failure"},
                    {"IsCancelled", "is_cancelled"}, {"Error", "error"}, {"Value", "value"}
                };
                return "(" + receiver + ")." + names.at(value->text) + "()";
            }
            if (value->resolution == "cancellation")
                return "(" + receiver + ").is_cancelled()";
            return "(" + receiver + ")." + value->text;
        }
        if (value->kind == "binary") {
            const auto left = expression(value->children[0]);
            const auto right = expression(value->children[1]);
            if (value->text == "&&" || value->text == "||")
                return "(" + left + " " + value->text + " " + right + ")";
            std::string operation = "pl_left " + value->text + " pl_right";
            const bool comparison = value->text == "==" || value->text == "!=" ||
                value->text == "<" || value->text == "<=" ||
                value->text == ">" || value->text == ">=";
            const auto& leftOperand = value->children[0];
            const auto& operandType = leftOperand->convertTo.name.empty()
                ? leftOperand->type : leftOperand->convertTo;
            if (!standaloneHeader_ && comparison && (operandType.name == "float" || operandType.name == "float32")) {
                // The result type is bool; floating operands identify this boundary.
                // Conditional volatile reads keep a skipped comparison from raising
                // native status flags. A volatile result alone permits speculation
                // of the comparison before its conditionally executed store.
                const auto nativeType = cpp(operandType);
                return "([&]() { volatile " + nativeType + " pl_left = " + left +
                    "; volatile " + nativeType + " pl_right = " + right +
                    "; return " + operation + "; }())";
            }
            if (value->type.name != "float" && value->type.name != "float32" && (value->text == "+" || value->text == "-" || value->text == "*" || value->text == "/" || value->text == "%")) {
                const std::map<std::string, std::string> functions = {
                    {"+", value->type.name == "string" ? "concatenate" : "add"},
                    {"-", "subtract"}, {"*", "multiply"}, {"/", "divide"}, {"%", "remainder"}
                };
                operation = "pipelang::model::" + functions.at(value->text) + "(pl_left, pl_right)";
            }
            if (value->text == "&" || value->text == "|" || value->text == "^" ||
                value->text == "<<" || value->text == ">>") {
                const std::map<std::string, std::string> functions{
                    {"&", "bitAnd"}, {"|", "bitOr"}, {"^", "bitXor"},
                    {"<<", "shiftLeft"}, {">>", "shiftRight"}
                };
                operation = "pipelang::model::" + functions.at(value->text) + "(pl_left, pl_right)";
            }
            if (standaloneHeader_ &&
                (evaluationOrderNeutral(value->children[0]) || evaluationOrderNeutral(value->children[1]))) {
                // Replace the original slots right first; nested emitted operands
                // may themselves contain names from sequenced lambda lowering.
                operation.replace(operation.find("pl_right"), 8, "(" + right + ")");
                operation.replace(operation.find("pl_left"), 7, "(" + left + ")");
                return "(" + operation + ")";
            }
            if (!standaloneHeader_ && numericEmission_ && program_.activeHostNumericEnvironment &&
                (value->type.name == "float" || value->type.name == "float32")) {
                static const std::map<std::string, std::string> functions{
                    {"+", "activeAdd"}, {"-", "activeSubtract"},
                    {"*", "activeMultiply"}, {"/", "activeDivide"}
                };
                return "pipelang::model::" + functions.at(value->text) +
                    "(pipelang::model::ActiveNumericOperands<" + cpp(value->type) + ">{" + left + ", " + right + "})";
            }
            if (!standaloneHeader_ && value->type.name == "float32") {
                if (program_.projectEnabled) {
                    static const std::map<std::string, std::string> functions{
                        {"+", "addBinary32"}, {"-", "subtractBinary32"},
                        {"*", "multiplyBinary32"}, {"/", "divideBinary32"}
                    };
                    // The checker requires both arithmetic operands to be float32.
                    // A braced aggregate preserves left-to-right evaluation and
                    // first-failure stopping without one closure type per node.
                    return "pipelang::model::" + functions.at(value->text) +
                        "(pipelang::model::Binary32Operands{" + left + ", " + right + "})";
                }

                // A materialized binary32 result is a language rounding boundary;
                // native contraction must not fuse adjacent source operations.
                return "([&]() { auto pl_left = " + left + "; auto pl_right = " + right +
                    "; volatile float pl_result = " + operation + "; return pl_result; }())";
            }
            return "([&]() { auto pl_left = " + left + "; auto pl_right = " + right + "; return " + operation + "; }())";
        }
        if (value->kind == "conditional") {
            // Native conditional evaluation is lazy and evaluates the condition
            // exactly once. The explicit value conversion also owns record or
            // Result arms when either expression denotes an existing lvalue.
            const auto conditional = "((" + expression(value->children[0]) +
                ") ? (" + expression(value->children[1]) + ") : (" + expression(value->children[2]) + "))";
            // Both absent arms carry only an internal marker until the known
            // nullable target owns conversion. Never emit an authored C++ type
            // for that marker; condition/selected-arm evaluation stays lazy.
            if (value->type.name == "$null")
                return conditional;
            return "static_cast<" + cpp(value->type) + ">(" + conditional + ")";
        }
        if (value->kind == "named_construct") {
            if (standaloneHeader_) {
                const auto* record = findType(program_, value->type.name);
                bool ordered = record && record->fields.size() == value->children.size();
                for (std::size_t index = 0; ordered && index < value->children.size(); ++index)
                    ordered = record->fields[index].name == value->children[index]->text;
                if (ordered) {
                    std::string aggregate = value->text + "{";
                    for (std::size_t index = 0; index < value->children.size(); ++index) {
                        if (index != 0)
                            aggregate += ", ";
                        aggregate += expression(value->children[index]->children.front());
                    }
                    return aggregate + "}";
                }
            }
            std::string result = "([&]() { " + value->text + " pl_record{}; ";
            for (const auto& child : value->children)
                result += "pl_record." + child->text + " = " + expression(child->children[0]) + "; ";
            return result + "return pl_record; }())";
        }
        if (value->kind == "call") {
            if (const auto direct = directNumericCall(value))
                return *direct;
            const auto& target = value->children[0];
            std::string result = "([&]() { ";
            if (value->resolution == "host_sync" || value->resolution == "host_sync_value")
                result += "using pipelang::model::validate; auto pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution == "sync_release" || value->resolution == "sync_wait" || value->resolution == "sync_wait_timed")
                result += "auto pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution.starts_with("context_") && value->resolution != "context_construct")
                result += "auto pl_receiver = " + expression(target->children[0]) + "; pipelang::model::validate(pl_receiver); ";
            else if (value->resolution.starts_with("concurrent_read_") || value->resolution.starts_with("concurrent_mutate_"))
                result += "auto pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution.starts_with("atomic_") && value->resolution != "atomic_construct")
                result += "const auto& pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution.starts_with("collection_mutate_"))
                result += "auto& pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution.starts_with("collection_read_"))
                result += "auto pl_receiver = " + expression(target->children[0]) + "; ";
            else if (value->resolution == "list_at") {
                const auto& receiver = target->children[0];
                const bool borrow = stableOwnedListReceiver(receiver) && value->children.size() == 2 &&
                    readOnlyListIndex(value->children[1]);
                result += std::string(borrow ? "const auto& pl_receiver = " : "auto pl_receiver = ") + expression(receiver) + "; ";
            }
            else if (value->resolution == "list_add")
                result += "auto& pl_receiver = " + expression(target->children[0]) + "; ";
            std::string arguments;
            for (std::size_t index = 1; index < value->children.size(); ++index) {
                const auto name = "pl_argument" + std::to_string(index);
                auto argument = expression(value->children[index]);
                const auto& argumentType = value->children[index]->type.name;
                if (!standaloneHeader_ && numericEmission_ && program_.activeHostNumericEnvironment &&
                    (value->resolution == "float_math" || value->resolution == "numeric_conversion" ||
                     value->resolution == "numeric_round") &&
                    (argumentType == "float" || argumentType == "float32"))
                    argument = "pipelang::model::activeFloatingValue(" + argument + ")";
                result += "auto " + name + " = " + argument + "; ";
                if (value->resolution == "host_sync" || value->resolution == "host_sync_value")
                    result += "validate(" + name + "); ";
                if (!arguments.empty())
                    arguments += ", ";
                arguments += "std::move(" + name + ")";
            }
            if (value->resolution == "host_sync_value") {
                result += "if (!pl_receiver) throw pipelang::model::ExecutionFailure(\"null host service\"); ";
                result += "auto pl_value = " + scopeExpression() + ".invokeSynchronousHost([&]() { return pl_receiver->" +
                    target->text + "(" + arguments + "); }); ";
                if (usesType("float32"))
                    result += "pipelang::model::requireBinary32Environment(); ";
                result += "return pl_value; ";
            }
            else if (value->resolution == "host_sync") {
                result += "if (!pl_receiver) throw pipelang::model::ExecutionFailure(\"null host service\"); ";
                result += "return " + scopeExpression() + ".invokeSynchronousHost([&]() { return pl_receiver->" +
                    target->text + "(" + arguments + "); }); ";
            }
            else if (value->resolution == "string_byte_length_is")
                result += "return pipelang::model::byteLengthIs(" + arguments + "); ";
            else if (value->resolution == "string_byte_at_or")
                result += "return pipelang::model::byteAtOr(" + arguments + "); ";
            else if (value->resolution == "numeric_round") {
                // Native admission already proves the complete signed64 domain.
                // Repeating the ordinary guard could add floating status flags.
                if (numericEmission_)
                    result += "return static_cast<std::int64_t>(std::llround(" + arguments + ")); ";
                else
                    result += "return pipelang::model::roundToInt(" + arguments + "); ";
            }
            else if (value->resolution == "string_starts_with")
                result += "return pl_argument1.starts_with(pl_argument2); ";
            else if (value->resolution == "string_utf8_length")
                result += "return pipelang::model::utf8Length(" + arguments + "); ";
            else if (value->resolution == "string_from_int")
                result += "return pipelang::model::stringFromInt(" + arguments + "); ";
            else if (value->resolution == "string_utf8_slice")
                result += "return pipelang::model::utf8Slice(" + arguments + "); ";
            else if (value->resolution == "string_utf8_index_of")
                result += "return pipelang::model::utf8IndexOf(" + arguments + "); ";
            else if (value->resolution == "error_construct")
                result += "return pipelang::model::makeError(" + arguments + "); ";
            else if (value->resolution == "construct")
                result += "return " + target->text + "{" + arguments + "}; ";
            else if (numericEmission_ && value->resolution == "qualified_pure")
                result += "return " + numericFunctions_.at(value->callable) + "(" + arguments + "); ";
            else if (value->resolution == "qualified_pure" || (qualifiedEmission_ && value->resolution == "pure")) {
                const auto& function = qualifiedFunctions_.at(value->callable);
                std::string context = "pl_budget";
                if (program_.objectEnabled)
                    context += ", pl_execution";
                result += "return " + function.name + "(" + context + (arguments.empty() ? "" : ", " + arguments) + "); ";
            }
            else if (numericEmission_ && (value->resolution == "pure" ||
                (observationEmission_ && value->resolution == "effect")))
                result += "return " + target->text + "(" + arguments + "); ";
            else if (workerEmission_ && (value->resolution == "pure" || value->resolution == "effect"))
                result += "return pl_worker_" + target->text + "(" + state() + budgetArgument() + (arguments.empty() ? "" : ", " + arguments) + "); ";
            else if (value->resolution == "pure")
                result += "return pl_read_" + target->text + "(" + state() + budgetArgument() + (arguments.empty() ? "" : ", " + arguments) + "); ";
            else if (value->resolution == "effect")
                result += "return pl_effect_" + target->text + "(pl_transaction" + budgetArgument() + (arguments.empty() ? "" : ", " + arguments) + "); ";
            else if (value->resolution == "context_construct")
                result += "return " + scopeExpression() + ".createContextOwner(); ";
            else if (value->resolution == "context_replace")
                result += "(void)pl_receiver->replace(); ";
            else if (value->resolution == "context_invalidate")
                result += "pl_receiver->invalidate(); ";
            else if (value->resolution == "context_close")
                result += "pl_receiver->close(); ";
            else if (value->resolution == "sync_construct" || value->resolution == "sync_lock_construct")
                result += "return " + cpp(value->type) + "::Create(" + arguments + "); ";
            else if (value->resolution == "sync_release")
                result += "return pl_receiver.release(" + arguments + "); ";
            else if (value->resolution == "sync_wait" || value->resolution == "sync_wait_timed") {
                std::string timeout = "-1";
                std::string cancellation = workerEmission_ ? "pipelang::model::CancellationToken{}" : "pl_parallelCancellation";
                for (std::size_t index = 1; index < value->children.size(); ++index) {
                    const auto name = "pl_argument" + std::to_string(index);
                    if (value->children[index]->type.name == "CancellationToken")
                        cancellation = "pipelang::model::CancellationToken::combine(" + cancellation + ", " + name + ")";
                    else
                        timeout = name;
                }
                if (value->type.name != "void")
                    result += "return ";
                result += "pl_receiver.wait(pl_budget, " + timeout + ", " + cancellation + "); ";
            }
            else if (value->resolution == "concurrent_construct")
                result += "return " + cpp(value->type) + "::Empty(); ";
            else if (value->resolution.starts_with("concurrent_mutate_"))
                result += "return pl_receiver." + value->resolution.substr(18) + "(" + arguments + "); ";
            else if (value->resolution.starts_with("concurrent_read_"))
                result += "return pl_receiver." + value->resolution.substr(16) + "(" + arguments + "); ";
            else if (value->resolution == "atomic_construct")
                result += "return " + cpp(value->type) + "::Create(" + arguments + "); ";
            else if (value->resolution.starts_with("atomic_")) {
                if (usesBudget_ && (value->resolution == "atomic_add" || value->resolution == "atomic_increment" || value->resolution == "atomic_decrement"))
                    arguments += (arguments.empty() ? "" : ", ") + std::string("&pl_budget");
                result += "return pl_receiver." + value->resolution.substr(7) + "(" + arguments + "); ";
            }
            else if (value->resolution == "pair_construct")
                result += "return " + cpp(value->type) + "{" + arguments + "}; ";
            else if (value->resolution == "collection_construct")
                result += "return " + cpp(value->type) + "::" + target->text + "(" + arguments + "); ";
            else if (value->resolution.starts_with("collection_mutate_"))
                result += "return pl_receiver." + value->resolution.substr(18) + "(" + arguments + "); ";
            else if (value->resolution.starts_with("collection_read_"))
                result += "return pl_receiver." + value->resolution.substr(16) + "(" + arguments + "); ";
            else if (value->resolution == "list_construct")
                result += "return " + cpp(value->type) + "::Empty(); ";
            else if (value->resolution == "list_at" || value->resolution == "list_add")
                result += "return pl_receiver." + std::string(value->resolution == "list_at" ? "at" : "add") + "(" + arguments + "); ";
            else if (value->resolution == "float_bits")
                result += "return std::bit_cast<" + cpp(value->type) + ">(" + arguments + "); ";
            else if (value->resolution == "float_math") {
                const auto call = floatingMathCall(target->text, arguments);
                if (!standaloneHeader_ && value->type.name == "float32") {
                    result += "volatile float pl_result = " + call + "; return pl_result; ";
                } else {
                    result += "return " + call + "; ";
                }
            }
            else if (value->resolution == "unsigned_wrapping") {
                const std::map<std::string, std::string> functions{
                    {"WrappingAdd", "wrappingAdd"}, {"WrappingSubtract", "wrappingSubtract"},
                    {"WrappingMultiply", "wrappingMultiply"}
                };
                result += "return pipelang::model::" + functions.at(target->text) + "(" + arguments + "); ";
            }
            else if (value->resolution == "unsigned_conversion") {
                if (value->type.name == "int") {
                    result += "return pipelang::model::widenUInt32(" + arguments + "); ";
                } else if (value->type.name == "float" || value->type.name == "float32") {
                    result += "return pipelang::model::roundUInt32<" + cpp(value->type) + ">(" + arguments + "); ";
                } else if (target->text == "FromInt") {
                    result += "return pipelang::model::checkedUInt32(" + arguments + "); ";
                } else {
                    result += "return pipelang::model::truncateToUInt32(" + arguments + "); ";
                }
            }
            else if (value->resolution == "numeric_conversion") {

                if (standaloneHeader_) {
                    result += "return static_cast<" + cpp(value->type) + ">(" + arguments + "); ";
                } else if (value->type.name == "int") {
                    result += "return pipelang::model::truncateToInt(" + arguments + "); ";
                } else if (target->text == "FromInt") {
                    const auto conversion = numericEmission_ && program_.activeHostNumericEnvironment
                        ? "activeFromInteger" : "roundInteger";
                    result += "return pipelang::model::" + std::string(conversion) + "<" + cpp(value->type) + ">(" + arguments + "); ";
                } else if (value->type.name == "float32") {
                    const auto conversion = numericEmission_ && program_.activeHostNumericEnvironment
                        ? "activeNarrowFloat" : "narrowFloat";
                    result += "return pipelang::model::" + std::string(conversion) + "(" + arguments + "); ";
                } else {
                    result += "return pipelang::model::widenFloat(" + arguments + "); ";
                }
            }
            else if (value->resolution == "optional_construct")
                result += "return " + cpp(value->type) + "::" + target->text + "(" + arguments + "); ";
            else if (value->resolution == "result_construct")
                result += "return " + cpp(value->type) + "::" + target->text + "(" + arguments + "); ";
            else
                throw Diagnostic("unsupported emitted call", value->offset);
            return result + "}())";
        }
        throw Diagnostic("unsupported emitted expression", value->offset);
    }
    void validateParameters(const Method& method, unsigned indentation, bool environmentIndependent = false) {
        if (usesType("float32") && !environmentIndependent)
            line("pipelang::model::requireBinary32Environment();", indentation);
        if (program_.objectEnabled)
            line("using pipelang::model::validate;", indentation);
        line("using pipelang::model::validate;", indentation);
        for (const auto& parameter : method.parameters) {
            if (service(program_, parameter.type)) {
                line("if (!" + parameter.name + ")", indentation);
                line("throw pipelang::model::ExecutionFailure(\"null host service\");", indentation + 1);
            } else {
                line("validate(" + parameter.name + ");", indentation);
                if (program_.objectEnabled && referenceBearing(parameter.type)) {
                    const auto domain = objectEmission_ ? objectDomain() : "pl_objects.domain()";
                    line("(void)" + objectStorage(parameter.type) + "::encode(" + parameter.name + ", " + domain + ");", indentation);
                }
            }
        }
    }
    Type actionResult(const Method& method) const {
        // String errors share the established invocation boundary. A typed domain
        // error cannot represent runtime faults, so preserve it inside that boundary.
        if (method.result.name == "Result" && method.result.arguments[1].name == "string")
            return method.result;
        return {"Result", {method.result, {"string", {}}}};
    }
    void emitReturnValue(const std::string& value, bool hasValue, unsigned indentation) {
        if (const auto* region = asyncFinally_.returning()) {
            line(region->exit + ".returning(" + value + ");", indentation);
            line("goto " + region->label + ";", indentation);
            return;
        }
        if (method_->pure || internalEffect_ || observationEmission_) {
            line("return" + (hasValue ? " " + value : "") + ";", indentation);
            return;
        }
        if (hasValue)
            line("auto pl_return = " + value + ";", indentation);
        if (method_->asynchronous && !program_.objectEnabled && hasFinally(method_->body))
            line("pl_returnCommit.arm();", indentation);
        else
            line(commitStatement(), indentation);
        if (method_->constructor)
            line("return;", indentation);
        else if (method_->asynchronous)
            line("co_return pl_return;", indentation);
        else if (actionResult(*method_) == method_->result)
            line("return pl_return;", indentation);
        else
            line("return " + cpp(actionResult(*method_)) + "::Success(" +
                (hasValue ? "std::move(pl_return)" : "") + ");", indentation);
    }

    bool hasPropagation(const Expr& value) const {
        return value && (value->kind == "propagate" ||
            std::any_of(value->children.begin(), value->children.end(),
                [&](const Expr& child) { return hasPropagation(child); }));
    }

    // Each statement owns its substitutions. Nested branch/loop scopes restore
    // their parent's values, so no generated local can escape its lexical scope.
    struct PropagationBindings {
        std::map<const Expression*, std::string>& values;
        std::map<const Expression*, std::string> previous;
        explicit PropagationBindings(std::map<const Expression*, std::string>& bindings)
            : values(bindings), previous(bindings) {}
        ~PropagationBindings() { values = std::move(previous); }
    };

    std::string propagationTemporary() {
        return "pl_propagated" + std::to_string(propagationSequence_++);
    }

    void prepareExpression(const Expr& value, unsigned indentation) {
        if (hasPropagation(value))
            materializeExpression(value, indentation);
    }

    std::optional<std::string> modelFieldLocation(const Expr& value) {
        if (value->resolution == "field")
            return state() + "." + value->text;
        if (value->kind == "member" && value->resolution == "record") {
            if (const auto owner = modelFieldLocation(value->children[0]))
                return *owner + "." + value->text;
        }
        return std::nullopt;
    }

    void materializeExpression(const Expr& value, unsigned indentation, bool borrow = false) {
        if (propagationValues_.contains(value.get()))
            return;
        const auto temporary = propagationTemporary();
        if (!hasPropagation(value)) {
            if (borrow) {
                if (const auto location = modelFieldLocation(value)) {
                    // Charge/evaluate the source location before its RHS, but
                    // reacquire transactional storage after any await/refresh.
                    line("(void)(" + expression(value) + ");", indentation);
                    propagationValues_[value.get()] = *location;
                    return;
                }
            }
            line(std::string(borrow ? "auto& " : "auto ") + temporary + " = " + expression(value) + ";", indentation);
            propagationValues_[value.get()] = temporary;
            return;
        }
        if (usesBudget_)
            line("pl_budget.step();", indentation);
        if (value->kind == "await") {
            Statement awaited;
            awaited.kind = value->type.name == "void" ? "expression" : "local";
            awaited.name = temporary;
            awaited.type = value->type;
            awaited.expression = value;
            prepareCallOperands(value->children[0], indentation);
            emitAwait(awaited, indentation);
            auto awaitedValue = value->type.name == "void" ? "(void)0" : temporary;
            if (!value->convertTo.name.empty())
                awaitedValue = convertedExpression(std::move(awaitedValue), value->type, value->convertTo);
            propagationValues_[value.get()] = std::move(awaitedValue);
            return;
        }
        if (value->kind == "propagate") {
            const auto& operand = value->children[0];
            if (operand->kind == "await") {
                Statement awaited;
                awaited.kind = "local";
                awaited.name = temporary;
                awaited.type = operand->type;
                awaited.expression = operand;
                // Preserve receiver/argument evaluation before suspension. The
                // established await emitter owns task lifetime and transaction segments.
                prepareCallOperands(operand->children[0], indentation);
                emitAwait(awaited, indentation);
            } else {
                prepareExpression(operand, indentation);
                line("auto " + temporary + " = " + expression(operand) + ";", indentation);
            }
            line("if (!" + temporary + ".is_success()) {", indentation);
            const auto& returned = method_->asynchronous ? method_->result.arguments[0] : method_->result;
            emitReturnValue("(" + temporary + ".is_cancelled() ? " + cpp(returned) +
                "::Cancelled(" + temporary + ".error()) : " + cpp(returned) +
                "::Failure(" + temporary + ".error()))", true, indentation + 1);
            line("}", indentation);
            auto payload = value->type.name == "void" ? "(void)0" : temporary + ".value()";
            if (!value->convertTo.name.empty())
                payload = convertedExpression(std::move(payload), value->type, value->convertTo);
            propagationValues_[value.get()] = std::move(payload);
            return;
        }
        if (value->kind == "conditional" || (value->kind == "binary" &&
            (value->text == "&&" || value->text == "||"))) {
            materializeExpression(value->children[0], indentation);
            const auto condition = expression(value->children[0]);
            const auto targetType = value->convertTo.name.empty() ? value->type : value->convertTo;
            line("std::optional<" + cpp(targetType) + "> " + temporary + ";", indentation);
            line("if (" + condition + ") {", indentation);
            const auto common = propagationValues_;
            const auto branch = [&](const Expr& arm, unsigned depth) {
                materializeExpression(arm, depth);
                auto emitted = expression(arm);
                const auto actual = arm->convertTo.name.empty() ? arm->type : arm->convertTo;
                emitted = convertedExpression(std::move(emitted), actual, targetType);
                line(temporary + ".emplace(" + emitted + ");", depth);
            };
            if (value->kind == "conditional")
                branch(value->children[1], indentation + 1);
            else if (value->text == "&&")
                branch(value->children[1], indentation + 1);
            else
                line(temporary + ".emplace(" + convertedExpression("true", Type{"bool", {}}, targetType) + ");", indentation + 1);
            propagationValues_ = common;
            line("} else {", indentation);
            if (value->kind == "conditional")
                branch(value->children[2], indentation + 1);
            else if (value->text == "||")
                branch(value->children[1], indentation + 1);
            else
                line(temporary + ".emplace(" + convertedExpression("false", Type{"bool", {}}, targetType) + ");", indentation + 1);
            propagationValues_ = common;
            line("}", indentation);
            propagationValues_[value.get()] = "(*" + temporary + ")";
            return;
        }
        if (value->kind == "call" || value->kind == "new_construct") {
            prepareCallOperands(value, indentation);
        } else if (value->kind == "named_construct") {
            for (const auto& field : value->children)
                materializeExpression(field->children[0], indentation);
        } else {
            for (const auto& child : value->children)
                materializeExpression(child, indentation, borrow);
        }
        auto emitted = expressionValue(value);
        if (!value->convertTo.name.empty())
            emitted = convertedExpression(std::move(emitted), value->type, value->convertTo);
        const auto type = value->convertTo.name.empty() ? value->type : value->convertTo;
        if (type.name == "void") {
            line(emitted + ";", indentation);
            propagationValues_[value.get()] = "(void)0";
        } else {
            line(std::string(borrow ? "auto& " : "auto ") + temporary + " = " + emitted + ";", indentation);
            propagationValues_[value.get()] = temporary;
        }
    }

    void prepareCallOperands(const Expr& call, unsigned indentation) {
        const auto& target = call->children[0];
        if ((target->kind == "member" || target->kind == "generic_member") &&
            !(workerEmission_ && call->statelessWorkerCall)) {
            const auto& receiver = target->children[0];
            if (receiver->kind != "type" && !receiver->type.name.empty()) {
                const bool mutating = call->resolution == "list_add" || call->resolution.starts_with("collection_mutate_");
                const auto objectField = mutating ? objectFieldRoot(receiver) : Expr{};
                if (objectField) {
                    if (objectField->kind == "member")
                        materializeExpression(objectField->children[0], indentation);
                } else {
                    materializeExpression(receiver, indentation, mutating);
                }
            }
        }
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            materializeExpression(call->children[index], indentation);
            if (call->resolution == "host_sync" || call->resolution == "host_sync_value" || call->resolution == "host") {
                line("{", indentation);
                line("using pipelang::model::validate;", indentation + 1);
                line("validate(" + expression(call->children[index]) + ");", indentation + 1);
                line("}", indentation);
            }
        }
    }

    bool continuesCurrentLoop(const std::vector<Statement>& body) const {
        for (const auto& statement : body) {
            if (statement.kind == "continue")
                return true;
            if (statement.kind == "while" || statement.kind == "for" ||
                statement.kind == "foreach" || statement.kind == "parallel_foreach")
                continue;
            if (continuesCurrentLoop(statement.body) || continuesCurrentLoop(statement.alternative))
                return true;
        }
        return false;
    }

    void statements(const std::vector<Statement>& body, unsigned indentation) {
        for (const auto& statement : body) {
            PropagationBindings bindings(propagationValues_);
            if (usesBudget_)
                line("pl_budget.step();", indentation);
            const bool propagating = hasPropagation(statement.expression) || hasPropagation(statement.target);
            if (propagating && statement.kind == "local") {
                // Keep expression temporaries at full-expression lifetime. A
                // staging slot admits non-default-constructible Result values;
                // the authored local owns the moved value after the slot resets.
                const auto staging = propagationTemporary();
                line("std::optional<" + cpp(statement.type) + "> " + staging + ";", indentation);
                line("{", indentation);
                prepareExpression(statement.expression, indentation + 1);
                line(staging + ".emplace(" + expression(statement.expression) + ");", indentation + 1);
                line("}", indentation);
                line("[[maybe_unused]] " + cpp(statement.type) + " " + statement.name +
                    " = std::move(*" + staging + ");", indentation);
                line(staging + ".reset();", indentation);
                continue;
            }
            if (propagating) {
                line("{", indentation);
                ++indentation;
            }
            if (statement.kind != "while" && statement.kind != "for" && statement.kind != "assign")
                prepareExpression(statement.expression, indentation);
            if (statement.kind == "assign" && propagating) {
                const auto field = objectFieldRoot(statement.target);
                if (field) {
                    if (field->kind == "member")
                        materializeExpression(field->children[0], indentation);
                } else {
                    materializeExpression(statement.target, indentation, true);
                }
                prepareExpression(statement.expression, indentation);
            }
            if (statement.kind == "lock") {
                emitLock(statement, indentation);
            } else if (statement.kind == "parallel_foreach") {
                emitParallel(statement, indentation);
            } else if (statement.kind == "try") {
                emitTry(statement, indentation);
            } else if (statement.kind == "throw") {
                const auto error = statement.expression ? expression(statement.expression) : statement.name;
                line("pipelang::model::raiseError(" + error + ");", indentation);
            } else if (statement.kind == "break" || statement.kind == "continue") {
                emitLoopTransfer(statement.kind == "continue", indentation);
            } else if (statement.kind == "for") {
                if (numericEmission_ && !nativeProjection_.countedLoops.contains(&statement))
                    throw Diagnostic("numeric for lacks a bounded counter proof", statement.offset);
                const bool literalTrue = statement.expression->kind == "boolean" &&
                    statement.expression->text == "true";
                line("{", indentation);
                statements(statement.initialization, indentation + 1);
                const bool propagationHeader = hasPropagation(statement.expression) ||
                    std::any_of(statement.iteration.begin(), statement.iteration.end(),
                        [&](const Statement& iteration) {
                            return hasPropagation(iteration.expression) || hasPropagation(iteration.target);
                        });
                if (propagationHeader) {
                    const auto label = "pl_iteration" + std::to_string(propagationSequence_++);
                    line("while (true) {", indentation + 1);
                    if (literalTrue) {
                        if (usesBudget_)
                            line("pl_budget.step();", indentation + 2);
                    } else {
                        prepareExpression(statement.expression, indentation + 2);
                        line("if (!(" + expression(statement.expression) + ")) break;", indentation + 2);
                    }
                    line("{", indentation + 2);
                    const bool needsContinueLabel = continuesCurrentLoop(statement.body);
                    propagationContinueLabels_.push_back(needsContinueLabel ? label : "");
                    statements(statement.body, indentation + 3);
                    propagationContinueLabels_.pop_back();
                    line("}", indentation + 2);
                    if (needsContinueLabel)
                        line(label + ":;", indentation + 2);
                    statements(statement.iteration, indentation + 2);
                    line("}", indentation + 1);
                } else {
                    const auto condition = literalTrue ? "true" : expression(statement.expression);
                    line("for (; " + condition + "; [&]() {", indentation + 1);
                    statements(statement.iteration, indentation + 2);
                    line("}()) {", indentation + 1);
                    // Keep C++ non-fallthrough visible without losing the
                    // literal condition's evaluation tick on each iteration.
                    if (literalTrue && usesBudget_)
                        line("pl_budget.step();", indentation + 2);
                    propagationContinueLabels_.push_back("");
                    statements(statement.body, indentation + 2);
                    propagationContinueLabels_.pop_back();
                    line("}", indentation + 1);
                }
                line("}", indentation);
            } else if (statement.kind == "while") {
                if (numericEmission_ && !nativeProjection_.countedLoops.contains(&statement))
                    throw Diagnostic("numeric while lacks a bounded counter proof", statement.offset);
                const bool literalTrue = statement.expression->kind == "boolean" &&
                    statement.expression->text == "true";
                if (literalTrue) {
                    line("while (true) {", indentation);
                    if (usesBudget_)
                        line("pl_budget.step();", indentation + 1);
                } else if (hasPropagation(statement.expression)) {
                    line("while (true) {", indentation);
                    prepareExpression(statement.expression, indentation + 1);
                    line("if (!(" + expression(statement.expression) + ")) break;", indentation + 1);
                } else {
                    line("while (" + expression(statement.expression) + ") {", indentation);
                }
                propagationContinueLabels_.push_back("");
                statements(statement.body, indentation + 1);
                propagationContinueLabels_.pop_back();
                line("}", indentation);
            } else if (statement.kind == "foreach") {
                const auto suffix = std::to_string(loopSequence_++);
                const auto snapshot = "pl_items" + suffix;
                line("{", indentation);
                line("auto " + snapshot + " = " + expression(statement.expression) + ";", indentation + 1);
                if (numericEmission_) {
                    if (!nativeProjection_.arrayLoops.contains(&statement))
                        throw Diagnostic("numeric foreach lacks a fixed-length proof", statement.offset);
                    line("for (auto " + statement.name + " : " + snapshot + ") {", indentation + 1);
                } else if (!statement.itemConvertTo.name.empty()) {
                    const auto& sequence = statement.expression->type;
                    const auto element = sequenceElementType(sequence).value();
                    const auto input = sequence.name == "ByteBuffer" ? "static_cast<std::uint32_t>(pl_iterationValue)"
                                                                     : "pl_iterationValue";
                    line("for (auto pl_iterationValue : " + snapshot + ".values()) {", indentation + 1);
                    line("auto " + statement.name + " = " + convertedExpression(input, element, statement.type) + ";",
                         indentation + 2);
                } else if (statement.expression->type.name == "ByteBuffer") {
                    // Storage bytes are uint8_t; the language element is uint32.
                    // Keep that widening explicit for overloads and bitwise operations.
                    line("for (std::uint32_t " + statement.name + " : " + snapshot + ".values()) {", indentation + 1);
                } else {
                    line("for (auto " + statement.name + " : " + snapshot + ".values()) {", indentation + 1);
                }
                if (usesBudget_)
                    line("pl_budget.step();", indentation + 2);
                propagationContinueLabels_.push_back("");
                statements(statement.body, indentation + 2);
                propagationContinueLabels_.pop_back();
                line("}", indentation + 1);
                line("}", indentation);
            } else if (statement.kind == "block") {
                line("{", indentation);
                statements(statement.body, indentation + 1);
                line("}", indentation);
            } else if (statement.kind == "if") {
                auto condition = expression(statement.expression);
                if (standaloneHeader_) {
                    // Source conditions are already bool. Keep expression grouping
                    // without Clang treating equality parentheses as a likely typo.
                    condition = "static_cast<bool>(" + condition + ")";
                }

                line("if (" + condition + ") {", indentation);
                statements(statement.body, indentation + 1);
                if (!statement.alternative.empty()) {
                    line("} else {", indentation);
                    statements(statement.alternative, indentation + 1);
                }
                line("}", indentation);
            } else if (statement.kind == "return") {
                emitReturnValue(statement.expression ? expression(statement.expression) : "",
                    static_cast<bool>(statement.expression), indentation);
            } else if (statement.kind == "expression" && statement.expression->kind != "await") {
                line(expression(statement.expression) + ";", indentation);
            } else if (statement.expression->kind == "await") {
                if (propagationValues_.contains(statement.expression.get()))
                    storeAwaitedValue(statement, expression(statement.expression), indentation);
                else
                    emitAwait(statement, indentation);
            } else if (statement.kind == "assign" && objectFieldRoot(statement.target)) {
                emitObjectAssignment(statement.target, expression(statement.expression), indentation);
            } else if (statement.kind == "local") {
                if (numericEmission_ && statement.type.name == "Array") {
                    const auto proof = nativeProjection_.arrayLocals.find(&statement);
                    if (proof == nativeProjection_.arrayLocals.end())
                        throw Diagnostic("numeric array local lacks an inline extent proof", statement.offset);
                    line("[[maybe_unused]] std::array<" + cpp(statement.type.arguments.front()) + ", " + std::to_string(proof->second) +
                        "> " + statement.name + " = " + expression(statement.expression) + ";", indentation);
                } else {
                    line("[[maybe_unused]] " + cpp(statement.type) + " " + statement.name + " = " + expression(statement.expression) + ";", indentation);
                }
            } else {
                emitValueAssignment(statement.target, expression(statement.expression), indentation);
            }
            if (propagating) {
                --indentation;
                line("}", indentation);
            }
        }
    }
    void emitValueAssignment(const Expr& target, const std::string& value, unsigned indentation) {
        if (!usesBudget_) {
            line(expression(target) + " = " + value + ";", indentation);
            return;
        }
        // C++ assignment evaluates its RHS first. Bind the checked source LHS
        // before evaluating an effectful RHS, including all target budget ticks.
        // No cancellation/evaluation step may lose an authoritative returned
        // capability between that return and assignment to the protected local.
        line("{", indentation);
        line("auto& pl_assignmentTarget = " + expression(target) + ";", indentation + 1);
        line("pl_assignmentTarget = " + value + ";", indentation + 1);
        line("}", indentation);
    }
    void emitLock(const Statement& statement, unsigned indentation) {
        const auto suffix = std::to_string(lockSequence_++);
        const auto handle = "pl_lock" + suffix;
        const auto cancellation = workerEmission_ ? "pipelang::model::CancellationToken{}" : "pl_parallelCancellation";
        line("{", indentation);
        line("auto " + handle + " = " + expression(statement.expression) + ";", indentation + 1);
        line("[[maybe_unused]] auto pl_lockGuard" + suffix + " = " + handle +
             ".acquire(pl_budget, " + cancellation + ");", indentation + 1);
        statements(statement.body, indentation + 1);
        line("}", indentation);
    }
    void emitParallel(const Statement& statement, unsigned indentation) {
        const auto suffix = std::to_string(parallelSequence_++);
        const auto input = "pl_parallelInput" + suffix;
        const auto snapshot = "pl_parallelState" + suffix;
        line("{", indentation);
        line("auto " + input + " = " + expression(statement.expression) + ";", indentation + 1);
        // Ordinary-object worker bodies cannot read any owner field. Their
        // validated pure helpers require only value arguments and the worker
        // budget, so do not capture even an unused owner-state reference.
        std::string captures;
        if (program_.objectEnabled)
            captures = "pl_calls = pl_execution.calls()";
        if (!objectEmission_) {
            line("const auto& " + snapshot + " = " + state() + ";", indentation + 1);
            if (!captures.empty())
                captures += ", ";
            captures += "&" + snapshot;
        }
        const auto previousCaptures = workerCaptures_;
        for (const auto& capture : statement.parallelCaptures) {
            const auto alias = "pl_capture" + suffix + "_" + capture.name;
            line("const auto& " + alias + " = " + capture.name + ";", indentation + 1);
            if (!captures.empty())
                captures += ", ";
            captures += "&" + alias;
            workerCaptures_[capture.name] = alias;
        }
        line("pipelang::model::parallelForEach(" + input + ", pl_budget, pl_parallelCancellation, pl_parallelOptions,", indentation + 1);
        line("[" + captures + "]([[maybe_unused]] " + cpp(statement.type) + " " + statement.name +
             ", pipelang::model::EvaluationBudget& pl_budget) {", indentation + 2);
        if (!objectEmission_) {
            line("const auto& pl_workerState = " + snapshot + ";", indentation + 3);
            line("(void)pl_workerState;", indentation + 3);
        }
        line("pl_budget.step();", indentation + 3);
        workerEmission_ = true;
        statements(statement.body, indentation + 3);
        workerEmission_ = false;
        workerCaptures_ = previousCaptures;
        line("});", indentation + 2);
        line("}", indentation);
    }
    void emitLoopTransfer(bool continuing, unsigned indentation) {
        const auto* region = asyncFinally_.loopTransfer(propagationContinueLabels_.size(), continuing);
        if (region) {
            line(region->exit + (continuing ? ".continuing();" : ".breaking();"), indentation);
            line("goto " + region->label + ";", indentation);
        } else if (continuing && !propagationContinueLabels_.empty() &&
            !propagationContinueLabels_.back().empty()) {
            line("goto " + propagationContinueLabels_.back() + ";", indentation);
        } else {
            line(continuing ? "continue;" : "break;", indentation);
        }
    }
    void emitResumableTry(const Statement& statement, unsigned indentation) {
        const auto region = asyncFinally_.enter(propagationContinueLabels_.size());
        std::ostringstream protectedBody;
        {
            struct OutputRestoration {
                std::ostream*& selected;
                std::ostream* previous;
                ~OutputRestoration() { selected = previous; }
            } restoreOutput{selectedOutput_, selectedOutput_};
            selectedOutput_ = &protectedBody;
            // Emit once so the existing transfer-routing seam determines storage.
            // Nested regions restore this buffer; diagnostics restore its owner.
            line("try {", indentation + 1);
            emitProtectedTry(statement, indentation + 1, std::to_string(cleanupSequence_++));
            line("} catch (...) {", indentation + 1);
            line(region.exit + ".captureFailure();", indentation + 2);
            line("}", indentation + 1);
            // The label remains outside protected lexical scopes: a transfer
            // destroys locals before cleanup and crosses no initialization.
            line("goto " + region.label + ";", indentation + 1);
            line(region.label + ":;", indentation + 1);
        }
        const auto completed = asyncFinally_.leave();
        line("{", indentation);
        const std::string storage = completed.returns ? "" : ", false";
        line("pipelang::model::AsyncFinallyExit<" + cpp(method_->result.arguments[0]) + storage + "> " + region.exit + ";", indentation + 1);
        *selectedOutput_ << protectedBody.view();
        line("try {", indentation + 1);
        line("auto pl_cleanupBudget = pl_budget.cleanup();", indentation + 2);
        line("auto pl_cleanupTask = " + scopeExpression() + ".enterCleanup(pl_taskCancellation);", indentation + 2);
        statements(statement.cleanup, indentation + 2);
        line("} catch (...) {", indentation + 1);
        line(region.exit + ".captureCleanupFailure();", indentation + 2);
        line("}", indentation + 1);
        line(region.exit + ".rethrowFailure();", indentation + 1);
        if (completed.returns) {
            line("if (" + region.exit + ".isReturn()) {", indentation + 1);
            emitReturnValue("std::move(" + region.exit + ".returnValue())", true, indentation + 2);
            line("}", indentation + 1);
        }
        if (completed.breaks) {
            line("if (" + region.exit + ".isBreak()) {", indentation + 1);
            emitLoopTransfer(false, indentation + 2);
            line("}", indentation + 1);
        }
        if (completed.continues) {
            line("if (" + region.exit + ".isContinue()) {", indentation + 1);
            emitLoopTransfer(true, indentation + 2);
            line("}", indentation + 1);
        }
        line("}", indentation);
    }
    void emitProtectedTry(const Statement& statement, unsigned indentation, const std::string& suffix) {
        if (statement.name.empty()) {
            line("{", indentation + 1);
            statements(statement.body, indentation + 2);
            line("}", indentation + 1);
        } else {
            const auto error = "pl_caught" + suffix;
            line("std::optional<pipelang::model::Error> " + error + ";", indentation + 1);
            line("try {", indentation + 1);
            statements(statement.body, indentation + 2);
            line("} catch (const pipelang::model::EvaluationLimitFailure&) {", indentation + 1);
            line("throw;", indentation + 2);
            line("} catch (const pipelang::model::ExecutionFailure& pl_failure) {", indentation + 1);
            line(error + " = pipelang::model::makeError(pipelang::model::safeErrorMessage(pl_failure));", indentation + 2);
            line("}", indentation + 1);
            // Own the error outside the native handler: source recovery may
            // suspend without retaining a reference to a native exception.
            if (statement.bodyFallsThrough)
                line("if (" + error + ") {", indentation + 1);
            else
                line("{", indentation + 1);
            line("[[maybe_unused]] auto " + statement.name + " = std::move(*" + error + ");", indentation + 2);
            statements(statement.alternative, indentation + 2);
            line("}", indentation + 1);
        }
    }
    void emitTry(const Statement& statement, unsigned indentation) {
        if (statement.hasFinally && program_.objectEnabled && method_->asynchronous && !workerEmission_) {
            emitResumableTry(statement, indentation);
            return;
        }
        const auto suffix = std::to_string(cleanupSequence_++);
        line("{", indentation);
        if (statement.hasFinally) {
            line("[[maybe_unused]] auto pl_finally" + suffix + " = pipelang::model::Finally(", indentation + 1);
            line("[&]() {", indentation + 2);
            line("auto pl_cleanupRegion = pl_budget.cleanup();", indentation + 3);
            statements(statement.cleanup, indentation + 3);
            line("},", indentation + 2);
            if (method_->asynchronous && !workerEmission_)
                line(std::string(objectEmission_ ? "[&]" : "[this]") +
                     "() noexcept { return " + scopeExpression() + ".abandoningFrames(); });", indentation + 2);
            else
                line("[]() noexcept { return false; });", indentation + 2);
        }
        emitProtectedTry(statement, indentation, suffix);
        line("}", indentation);
    }
    void storeAwaited(const Statement& statement, std::string value, unsigned indentation) {
        // A materialized await uses a temporary of the raw awaited type.
        // Authored locals and assignments use the checked destination type.
        if (statement.kind == "local")
            value = convertedExpression(std::move(value), statement.expression->type, statement.type);
        else if (statement.kind == "assign")
            value = convertedExpression(std::move(value), statement.expression->type, statement.target->type);
        storeAwaitedValue(statement, value, indentation);
    }
    void storeAwaitedValue(const Statement& statement, const std::string& value, unsigned indentation) {
        if (statement.kind == "local")
            line(cpp(statement.type) + " " + statement.name + " = " + value + ";", indentation);
        else if (statement.kind == "assign" && objectFieldRoot(statement.target))
            emitObjectAssignment(statement.target, value, indentation);
        else if (statement.kind == "assign")
            line(expression(statement.target) + " = " + value + ";", indentation);
    }
    void emitSemaphoreAwait(const Statement& statement, unsigned indentation) {
        const auto& call = statement.expression->children[0];
        const auto suffix = std::to_string(awaitSequence_++);
        const auto handle = "pl_semaphore" + suffix;
        const auto cancellation = "pl_semaphoreCancellation" + suffix;
        const auto operation = "pl_semaphoreOperation" + suffix;
        const auto suspension = "pl_semaphoreSuspension" + suffix;
        const auto result = "pl_semaphoreResult" + suffix;
        line("auto " + handle + " = " + expression(call->children[0]->children[0]) + ";", indentation);
        line("auto " + cancellation + " = pl_taskCancellation;", indentation);
        std::string timeout = "-1";
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            const auto name = "pl_semaphoreArgument" + suffix + "_" + std::to_string(index);
            line("auto " + name + " = " + expression(call->children[index]) + ";", indentation);
            line("validate(" + name + ");", indentation);
            if (call->children[index]->type.name == "CancellationToken")
                line(cancellation + " = pipelang::model::CancellationToken::combine(" + cancellation + ", " + name + ");", indentation);
            else
                timeout = name;
        }
        line("auto " + operation + " = " + handle + ".waitAsync(" + scopeExpression() + ", " + timeout + ", " + cancellation + ");", indentation);
        line("const auto " + suspension + " = " + operation + ".suspensionState();", indentation);
        line("[[maybe_unused]] bool " + result + " = false;", indentation);
        line(commitStatement(), indentation);
        line("try {", indentation);
        line(result + " = co_await std::move(" + operation + ");", indentation + 1);
        line("} catch (...) {", indentation);
        line(refreshStatement(), indentation + 1);
        line("if (*" + suspension + ")", indentation + 1);
        line("pl_budget.reset();", indentation + 2);
        line("throw;", indentation + 1);
        line("}", indentation);
        line(refreshStatement(), indentation);
        line("if (*" + suspension + ")", indentation);
        line("pl_budget.reset();", indentation + 1);
        storeAwaited(statement, result, indentation);
    }
    void emitAwait(const Statement& statement, unsigned indentation) {
        const auto& call = statement.expression->children[0];
        const auto& target = call->children[0];
        if (call->resolution == "object_async" || call->resolution == "static_async" || call->resolution == "source_interface_async" || call->resolution == "class_virtual_async") {
            emitObjectAwait(statement, indentation);
            return;
        }
        if (call->resolution == "sync_wait_async" || call->resolution == "sync_wait_async_timed") {
            emitSemaphoreAwait(statement, indentation);
            return;
        }
        if (call->resolution == "local_async") {
            emitLocalAwait(statement, indentation);
            return;
        }
        const auto suffix = std::to_string(awaitSequence_++);
        const auto host = "pl_host" + suffix;
        line("auto " + host + " = " + expression(target->children[0]) + ";", indentation);
        line("if (!" + host + ")", indentation);
        line("throw pipelang::model::ExecutionFailure(\"null host service\");", indentation + 1);
        std::string captures = host;
        std::string arguments;
        const auto cancellation = "pl_cancellation" + suffix;
        line("auto " + cancellation + " = pl_taskCancellation;", indentation);
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            const auto name = "pl_await" + suffix + "_argument" + std::to_string(index);
            line("auto " + name + " = " + expression(call->children[index]) + ";", indentation);
            line("validate(" + name + ");", indentation);
            if (call->children[index]->type.name == "CancellationToken")
                line(cancellation + " = pipelang::model::CancellationToken::combine(" + cancellation + ", " + name + ");", indentation);
            captures += ", " + name;
            if (!arguments.empty())
                arguments += ", ";
            arguments += "std::move(" + name + ")";
        }
        if (program_.objectEnabled) {
            line(cancellation + " = pipelang::model::CancellationToken::combine(" + cancellation +
                ", pipelang::model::currentPermit().cancellation());", indentation);
            for (std::size_t index = 1; index < call->children.size(); ++index) {
                if (call->children[index]->type.name == "CancellationToken") {
                    const auto name = "pl_await" + suffix + "_argument" + std::to_string(index);
                    line(name + " = " + cancellation + ";", indentation);
                }
            }
        }
        // Keep every owner-transfer boundary in a named coroutine local.
        // Nontrivial captured values must not depend on a temporary closure's
        // full-expression lifetime across suspension and worker execution.
        const auto function = "pl_await" + suffix + "_function";
        const auto operation = "pl_await" + suffix + "_operation";
        const auto temporary = "pl_await" + suffix + "_result";
        line("std::function<" + cpp(statement.expression->type) + "()> " + function +
             " = [" + captures + "]() mutable {", indentation);
        line("return " + host + "->" + target->text + "(" + arguments + ");", indentation + 1);
        line("};", indentation);
        line("pipelang::model::TaskScope::HostAwaitable<" + cpp(statement.expression->type) + "> " + operation +
             " = " + scopeExpression() + "." + objectTemplateKeyword() + "host<" + cpp(statement.expression->type) + ">(", indentation);
        line("std::move(" + function + "), std::move(" + cancellation + "));", indentation + 1);
        const auto suspension = "pl_await" + suffix + "_suspension";
        if (usesBudget_)
            line("const auto " + suspension + " = " + operation + ".suspensionState();", indentation);
        line(commitStatement(), indentation);
        line("auto " + temporary + " = co_await std::move(" + operation + ");", indentation);
        line(refreshStatement(), indentation);
        if (usesBudget_) {
            line("if (*" + suspension + ")", indentation);
            line("pl_budget.reset();", indentation + 1);
        }
        storeAwaited(statement, "std::move(" + temporary + ")", indentation);
    }
    void emitLocalAwait(const Statement& statement, unsigned indentation) {
        const auto& call = statement.expression->children[0];
        const auto suffix = std::to_string(awaitSequence_++);
        const auto cancellation = "pl_child" + suffix + "_cancellation";
        line("auto " + cancellation + " = pl_taskCancellation;", indentation);
        std::string arguments;
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            const auto name = "pl_child" + suffix + "_argument" + std::to_string(index);
            auto value = expression(call->children[index]);
            line("auto " + name + " = " + value + ";", indentation);
            if (!program_.objectEnabled && call->children[index]->type.name == "CancellationToken")
                line(cancellation + " = pipelang::model::CancellationToken::combine(" + cancellation + ", " + name + ");", indentation);
            if (!arguments.empty())
                arguments += ", ";
            arguments += "std::move(" + name + ")";
        }
        if (!arguments.empty())
            arguments += ", ";
        for (std::size_t index = 1; index < call->children.size(); ++index) {
            if (!program_.objectEnabled && call->children[index]->type.name == "CancellationToken") {
                const auto name = "pl_child" + suffix + "_argument" + std::to_string(index);
                line(name + " = " + cancellation + ";", indentation);
            }
        }
        arguments += cancellation + ", &pl_budget";
        if (program_.objectEnabled && !objectEmission_)
            arguments += ", &pl_transaction, pl_execution.calls()";
        line(commitStatement(), indentation);
        const auto operation = "pl_child" + suffix + "_operation";
        const auto result = "pl_child" + suffix + "_result";
        if (program_.objectEnabled) {
            line("std::optional<" + cpp(statement.expression->type) + "> " + result + ";", indentation);
            line("try {", indentation);
            line("auto " + operation + " = pl_async_" + call->children[0]->text + "(" + arguments + ");", indentation + 1);
            line(result + ".emplace(co_await std::move(" + operation + "));", indentation + 1);
            line("} catch (...) {", indentation);
            line(refreshStatement(), indentation + 1);
            line("throw;", indentation + 1);
            line("}", indentation);
            line(refreshStatement(), indentation);
            storeAwaited(statement, "std::move(*" + result + ")", indentation);
        } else {
            line("auto " + operation + " = pl_async_" + call->children[0]->text + "(" + arguments + ");", indentation);
            line("auto " + result + " = co_await std::move(" + operation + ");", indentation);
            line(refreshStatement(), indentation);
            storeAwaited(statement, "std::move(" + result + ")", indentation);
        }
    }
    void interface(const Declaration& declaration) {
        line("struct " + declaration.name + " {");
        line("    virtual ~" + declaration.name + "() = default;");
        for (const auto& method : declaration.methods) {
            const auto& result = method.asynchronous ? method.result.arguments[0] : method.result;
            line("    virtual " + cpp(result) + " " + method.name + "(" + parameters(method) + ")" + (method.pure ? " const" : "") + " = 0;");
        }
        line("};");
    }
    void model(const Declaration& declaration) {
        memberOwner_ = declaration.name;
        implementationOwner_ = declaration.name;
        classSynchronization_ = usesSynchronization();
        classParallel_ = (program_.objectEnabled && usesParallel()) || std::any_of(declaration.methods.begin(), declaration.methods.end(), [&](const Method& method) {
            return containsStatement(method.body, "parallel_foreach");
        });
        localAsync_ = program_.objectEnabled || std::any_of(declaration.methods.begin(), declaration.methods.end(), [&](const Method& method) {
            return usesLocalAsync(method.body);
        });
        usesBudget_ = !qualifiedFunctions_.empty() || program_.objectEnabled || classParallel_ || classSynchronization_ || usesType("Atomic") || usesConcurrentCollections() || localAsync_ || std::any_of(declaration.methods.begin(), declaration.methods.end(), [&](const Method& method) {
            return usesLoops(method.body) || containsStatement(method.body, "try") ||
                containsStatement(method.body, "throw");
        });
        line("class " + declaration.name + " {");
        line("public:");
        if (!declaration.sourceObjectFacet.empty())
            line("using Reference = pipelang::model::ObjectRef<" + objectTag(declaration.sourceObjectFacet) + ">;", 1);
        for (const auto& constant : declaration.constants) {
            if (!constant.visible)
                continue;
            const auto exports = constantsManifest(program_, nameSpace_);
            const auto identity = declaration.sourceName.empty() ? declaration.name : declaration.sourceName;
            const auto found = std::find_if(exports.begin(), exports.end(), [&](const auto& item) {
                return item.at("owner") == identity && item.at("name") == constant.name;
            });
            line("inline static constexpr " + cpp(constant.type) + " " + constant.name + " = " +
                found->at("cpp_name").template get<std::string>() + ";", 1);
        }

        line("struct State {", 1);
        for (const auto& field : declaration.fields) {
            if (field.visible && !service(program_, field.type))
                line(cpp(field.type) + " " + field.name + "{};", 2);
        }
        line("bool operator==(const State&) const = default;", 2);
        line("};", 1);
        line("private:");
        line("struct pl_State {", 1);
        for (const auto& field : declaration.fields) {
            if (!service(program_, field.type))
                line(cpp(field.type) + " " + field.name + "{};", 2);
        }
        line("};", 1);
        if (program_.objectEnabled) {
            line("friend void validate(const pl_State& value) {", 1);
            line("using pipelang::model::validate;", 2);
            line("(void)value;", 2);
            for (const auto& field : declaration.fields) {
                if (!service(program_, field.type))
                    line("validate(value." + field.name + ");", 2);
            }
            line("}", 1);
        }
        line("public:");
        for (const auto& method : declaration.methods) {
            method_ = &method;
            if (method.constructor)
                constructor(declaration, method);
        }
        line(declaration.name + "(const " + declaration.name + "&) = delete;", 1);
        line(declaration.name + "& operator=(const " + declaration.name + "&) = delete;", 1);
        if (compiled_) {
            line("~" + declaration.name + "();", 1);
            implementationUnits_->append(declaration.name,
                declaration.name + "::~" + declaration.name + "() = default;\n");
        }
        beginMember("State", "snapshot", "", " const", "", "", {}, declaration.name + "::State");
        line("pl_scope.requireOwner();", 2);
        std::string publicValues;
        for (const auto& field : declaration.fields) {
            if (field.visible && !service(program_, field.type)) {
                if (!publicValues.empty())
                    publicValues += ", ";
                publicValues += "pl_state." + field.name;
            }
        }
        line("return State{" + publicValues + "};", 2);
        endMember();
        if (usesType("float32")) {
            beginMember("std::size_t", "poll", "");
            line("pipelang::model::requireBinary32Environment();", 2);
            line("return pl_scope.poll();", 2);
            endMember();
        } else {
            if (compiled_) {
                beginMember("std::size_t", "poll", "");
                line("return pl_scope.poll();", 2);
                endMember();
            } else {
                line("std::size_t poll() { return pl_scope.poll(); }", 1);
            }
        }
        if (compiled_) {
            beginMember("std::size_t", "pending", "", " const");
            line("return pl_scope.pending();", 2);
            endMember();
        } else {
            line("std::size_t pending() const { return pl_scope.pending(); }", 1);
        }
        for (const auto& method : declaration.methods) {
            if (method.constructor || method.staticMethod || !method.visible)
                continue;
            method_ = &method;
            line(method.visible ? "public:" : "private:");
            if (method.pure) {
                beginMember(cpp(method.result), method.name, parameters(method), " const");
                line("pl_scope.requireOwner();", 2);
                validateParameters(method, 2);
                const auto arguments = argumentNames(method);
                declareBudget(2);
                cancellationContext(method, 2);
                if (program_.objectEnabled) {
                    line("pipelang::model::GraphTransaction pl_queryGraph(pl_objects.domain());", 2);
                    line("pipelang::model::ObjectExecution pl_execution(pl_scope, pl_budget, pl_queryGraph, {});", 2);
                }
                line("return pl_read_" + method.name + "(pl_state" + budgetArgument() + (arguments.empty() ? "" : ", " + arguments) + ");", 2);
                endMember();
            } else if (method.asynchronous) {
                asyncWrapper(method);
            } else {
                action(method);
            }
        }
        line("private:");
        for (const auto& method : declaration.methods) {
            if (method.staticMethod)
                continue;
            method_ = &method;
            if (method.pure) {
                beginMember(cpp(method.result), "pl_read_" + method.name, parameters(method, true),
                    usesSynchronousHosts() ? " const" : "", usesSynchronousHosts() ? "" : "static ");
                if (program_.objectEnabled) {
                    line("auto pl_execution = pl_parentExecution.enterCall();", 2);
                    line("pl_budget.step();", 2);
                }
                line("(void)pl_state;", 2);
                ignoreParallelParameters(2);
                unusedParameters(method, 2);
                statements(method.body, 2);
                if (containsStatement(method.body, "try") || containsStatement(method.body, "throw"))
                    line("throw pipelang::model::ExecutionFailure(\"unreachable compiler control flow\");", 2);
                endMember();
            } else if (!method.constructor && !method.asynchronous) {
                const auto parameterText = parameters(method);
                // Synchronous host effects use this model's fixed service and
                // owner scope. Older host-free/async-only output stays static.
                beginMember(cpp(method.result), "pl_effect_" + method.name,
                    transactionType() + "& pl_transaction" + (usesBudget_ ? ", pipelang::model::EvaluationBudget& pl_budget" : "") + parallelParameters() +
                    (program_.objectEnabled ? ", pipelang::model::ObjectExecution& pl_parentExecution" : "") +
                    (parameterText.empty() ? "" : ", " + parameterText), "", (usesSynchronousHosts() || usesType("ContextOwner")) ? "" : "static ");
                if (program_.objectEnabled) {
                    line("auto pl_execution = pl_parentExecution.enterCall();", 2);
                    line("pl_budget.step();", 2);
                }
                line("(void)pl_transaction;", 2);
                ignoreParallelParameters(2);
                unusedParameters(method, 2);
                internalEffect_ = true;
                statements(method.body, 2);
                internalEffect_ = false;
                if (method.result.name != "void" &&
                    (containsStatement(method.body, "try") || containsStatement(method.body, "throw")))
                    line("throw pipelang::model::ExecutionFailure(\"unreachable compiler control flow\");", 2);
                endMember();
            } else if (method.asynchronous) {
                const auto returnType = "pipelang::model::Coroutine<" + cpp(method.result.arguments[0]) + ">";
                const auto declarationParameters = parameters(method, false, true) +
                    (localAsync_ ? ", pipelang::model::EvaluationBudget* pl_parentBudget = nullptr" : "") +
                    (program_.objectEnabled ? ", pipelang::model::GraphModelTransaction<pl_State>* pl_parentTransaction = nullptr, pipelang::model::SourceCallContext pl_parentCalls = {}" : "");
                const auto definitionParameters = parameters(method, false, true) +
                    (localAsync_ ? ", pipelang::model::EvaluationBudget* pl_parentBudget" : "") +
                    (program_.objectEnabled ? ", pipelang::model::GraphModelTransaction<pl_State>* pl_parentTransaction, pipelang::model::SourceCallContext pl_parentCalls" : "");
                if (program_.objectEnabled) {
                    beginMember(returnType, "pl_async_" + method.name, declarationParameters, "", "", "", definitionParameters);
                    line("const auto pl_enteredCalls = pl_parentCalls.enter();", 2);
                    auto arguments = argumentNames(method);
                    if (!arguments.empty())
                        arguments += ", ";
                    arguments += "pl_taskCancellation, pl_parentBudget, pl_parentTransaction, pl_enteredCalls";
                    line("return pl_async_body_" + method.name + "(" + arguments + ");", 2);
                    endMember();
                }
                beginMember(returnType, (program_.objectEnabled ? "pl_async_body_" : "pl_async_") + method.name,
                    program_.objectEnabled ? definitionParameters : declarationParameters, "", "", "", definitionParameters);
                validateParameters(method, 2);
                if (!localAsync_ && (containsStatement(method.body, "try") || containsStatement(method.body, "throw")))
                    line("(void)pl_taskCancellation;", 2);
                if (localAsync_) {
                    line("(void)pl_taskCancellation;", 2);
                    line("auto pl_ownedBudget = pipelang::model::EvaluationBudget::forInvocation(pl_scope.evaluationStepsPerSegment());", 2);
                    line("auto& pl_budget = pl_parentBudget ? *pl_parentBudget : pl_ownedBudget;", 2);
                } else {
                    declareBudget(2);
                }
                cancellationContext(method, 2, true);
                if (program_.objectEnabled) {
                    line("std::optional<pipelang::model::GraphModelTransaction<pl_State>> pl_ownedTransaction;", 2);
                    line("if (!pl_parentTransaction)", 2);
                    line("pl_ownedTransaction.emplace(pl_state, pl_objects.domain());", 3);
                    line("auto& pl_transaction = pl_parentTransaction ? *pl_parentTransaction : *pl_ownedTransaction;", 2);
                    line("if (pl_parentTransaction)", 2);
                    line(refreshStatement(), 3);
                    // The ordinary factory entered before allocating this
                    // frame. The retained value survives every suspension.
                    emitRootExecution(2, true, "pl_parentCalls", true);
                } else {
                    line("pipelang::model::Transaction<pl_State> pl_transaction(pl_state);", 2);
                }
                if (!program_.objectEnabled && hasFinally(method.body))
                    line("pipelang::model::CommitOnReturn pl_returnCommit(pl_transaction, [this]() noexcept { return pl_scope.abandoningFrames(); });", 2);
                statements(method.body, 2);
                if (containsStatement(method.body, "try") || containsStatement(method.body, "throw"))
                    line("co_return " + cpp(method.result.arguments[0]) + "::Failure(\"unreachable compiler control flow\");", 2);
                endMember();
            }
        }
        for (const auto& method : declaration.methods) {
            if (!method.workerCallable)
                continue;
            method_ = &method;
            workerEmission_ = true;
            workerCaptures_.clear();
            const auto parameterText = parameters(method);
            beginMember(cpp(method.result), "pl_worker_" + method.name,
                std::string("const pl_State& pl_workerState, pipelang::model::EvaluationBudget& pl_budget") +
                (program_.objectEnabled ? ", pipelang::model::SourceCallContext pl_parentCalls" : "") +
                (parameterText.empty() ? "" : ", " + parameterText), "", "static ");
            if (program_.objectEnabled) {
                line("[[maybe_unused]] const auto pl_calls = pl_parentCalls.enter();", 2);
                line("pl_budget.step();", 2);
            }
            line("(void)pl_workerState;", 2);
            unusedParameters(method, 2);
            internalEffect_ = true;
            statements(method.body, 2);
            internalEffect_ = false;
            if (method.result.name != "void")
                line("throw pipelang::model::ExecutionFailure(\"unreachable worker control flow\");", 2);
            endMember();
            workerEmission_ = false;
        }
        if (program_.objectEnabled)
            line("pipelang::model::ObjectOwner pl_objects;", 1);
        line("pl_State pl_state;", 1);
        for (const auto& field : declaration.fields) {
            if (service(program_, field.type))
                line(cpp(field.type) + " pl_service_" + field.name + ";", 1);
        }
        // Reverse member destruction joins the scope before releasing services
        // or model state. Suspended frames never outlive this owner.
        if (classParallel_)
            line("pipelang::model::ParallelOptions pl_parallelOptions;", 1);
        line(std::string(program_.objectEnabled || usesSynchronousHosts() ? "mutable " : "") + "pipelang::model::TaskScope pl_scope;", 1);
        line("};");
    }
    void constructor(const Declaration& declaration, const Method& method) {
        auto parameterText = parameters(method);
        if (!parameterText.empty())
            parameterText += ", ";
        parameterText += "pipelang::model::TaskOptions pl_options = {}";
        if (classParallel_)
            parameterText += ", pipelang::model::ParallelOptions pl_parallel = {}";
        auto definitionParameters = parameters(method);
        if (!definitionParameters.empty())
            definitionParameters += ", ";
        definitionParameters += "pipelang::model::TaskOptions pl_options";
        if (classParallel_)
            definitionParameters += ", pipelang::model::ParallelOptions pl_parallel";
        beginMember("", declaration.name, parameterText, "", "explicit ",
            " : " + std::string(classParallel_ ? "pl_parallelOptions(pl_parallel), " : "") + "pl_scope(pl_options)",
            definitionParameters);
        if (classParallel_) {
            line("if (pl_parallelOptions.maximumDegreeOfParallelism == 0 || pl_parallelOptions.maximumDegreeOfParallelism > pipelang::model::ParallelOptions::maximumDegree)", 2);
            line("throw pipelang::model::ExecutionFailure(\"parallel degree must be between 1 and 32\");", 3);
        }
        validateParameters(method, 2);
        declareBudget(2, true);
        cancellationContext(method, 2);
        line(transactionType() + " pl_transaction(pl_state" + (program_.objectEnabled ? ", pl_objects.domain()" : "") + ");", 2);
        if (program_.objectEnabled)
            emitRootExecution(2, true);
        for (std::size_t index = 0; index < declaration.fields.size(); ++index) {
            const auto& initializer = method.fieldInitializers[index];
            if (initializer)
                line("pl_transaction.state()." + declaration.fields[index].name + " = " + expression(initializer) + ";", 2);
        }
        statements(method.body, 2);
        for (const auto& field : declaration.fields) {
            if (isSharedHandle(field.type))
                line("validate(pl_transaction.state()." + field.name + ");", 2);
            if (service(program_, field.type)) {
                line("if (!pl_service_" + field.name + ")", 2);
                line("throw pipelang::model::ExecutionFailure(\"constructor must initialize every host service\");", 3);
            }
        }
        line(commitStatement(), 2);
        endMember();
    }
    void action(const Method& method) {
        const auto result = cpp(actionResult(method));
        beginMember(result, method.name, parameters(method));
        line("pl_scope.requireOwner();", 2);
        line("try {", 2);
        validateParameters(method, 3, observationMethods_.contains(method.identity));
        declareBudget(3);
        cancellationContext(method, 3);
        line(transactionType() + " pl_transaction(pl_state" + (program_.objectEnabled ? ", pl_objects.domain()" : "") + ");", 3);
        if (program_.objectEnabled)
            emitRootExecution(3);
        const auto arguments = argumentNames(method);
        const auto invocation = "pl_effect_" + method.name + "(pl_transaction" + budgetArgument() +
                                (arguments.empty() ? "" : ", " + arguments) + ")";
        if (method.result.name == "void")
            line(invocation + ";", 3);
        else
            line("auto pl_return = " + invocation + ";", 3);
        line(commitStatement(), 3);
        if (actionResult(method) == method.result)
            line("return pl_return;", 3);
        else
            line("return " + result + "::Success(" + (method.result.name == "void" ? "" : "std::move(pl_return)") + ");", 3);
        line("} catch (const pipelang::model::StaleContextCancelled& pl_error) {", 2);
        line("return " + result + "::Cancelled(pipelang::model::safeErrorMessage(pl_error));", 3);
        if (classParallel_ || classSynchronization_ || usesSynchronousHosts()) {
            line("} catch (const pipelang::model::OperationCancelled& pl_error) {", 2);
            line("return " + result + "::Cancelled(pipelang::model::safeErrorMessage(pl_error));", 3);
        }
        line("} catch (const std::exception& pl_error) {", 2);
        line("return " + result + "::Failure(pipelang::model::safeErrorMessage(pl_error));", 3);
        line("}", 2);
        endMember();
    }
    void asyncWrapper(const Method& method) {
        beginMember(cpp(method.result), method.name, parameters(method));
        line("pl_scope.requireOwner();", 2);
        std::string capture = "this";
        for (const auto& parameter : method.parameters)
            capture += ", " + parameter.name + " = std::move(" + parameter.name + ")";
        line("return pl_scope.start<" + cpp(method.result.arguments[0]) + ">(", 2);
        line("[" + capture + "](pipelang::model::CancellationToken pl_taskCancellation) mutable {", 3);
        if (!program_.objectEnabled && (localAsync_ || classSynchronization_)) {
            for (const auto& parameter : method.parameters) {
                if (parameter.type.name == "CancellationToken")
                    line("pl_taskCancellation = pipelang::model::CancellationToken::combine(pl_taskCancellation, " + parameter.name + ");", 4);
            }
        }
        for (const auto& parameter : method.parameters) {
            if (!program_.objectEnabled && parameter.type.name == "CancellationToken")
                line(parameter.name + " = pipelang::model::CancellationToken::combine(" + parameter.name + ", pl_taskCancellation);", 4);
        }
        // Modern source arguments are values, not implicit task policy. Concrete
        // await/host/synchronization operations combine only their actual operands.
        auto arguments = argumentNames(method);
        if (!arguments.empty())
            arguments += ", ";
        line("return pl_async_" + method.name + "(" + arguments + "pl_taskCancellation);", 4);
        line("});", 3);
        endMember();
    }
    std::map<const Expression*, std::string> propagationValues_;
    std::vector<std::string> propagationContinueLabels_;
    std::size_t propagationSequence_ = 0;
    const Program& program_;
    mutable std::map<std::string, bool> typeUsage_;
    bool numericEmission_ = false;
    bool standaloneHeader_ = false;
    std::set<std::string> numericConstantMethods_;
    bool observationEmission_ = false;
    std::set<std::string> observationMethods_;
    bool qualifiedEmission_ = false;
    NativeNumericExports::Projection nativeProjection_;
    std::map<std::string, QualifiedFunction> qualifiedFunctions_;
    std::map<std::string, std::string> numericFunctions_;
    std::string nameSpace_;
    const Method* method_ = nullptr;
    std::ostringstream output_;
    bool compiled_ = false;
    std::ostringstream objects_;
    std::optional<CompiledImplementationUnits> implementationUnits_;
    std::ostringstream implementationDefinition_;
    std::string implementationDefinitionOwner_;
    bool implementationDefinitionActive_ = false;
    std::ostream* selectedOutput_ = &output_;
    std::ostream* memberHeader_ = nullptr;
    std::string memberOwner_;
    std::string implementationOwner_;
    std::size_t awaitSequence_ = 0;
    std::size_t loopSequence_ = 0;
    std::size_t cleanupSequence_ = 0;
    AsyncFinallyLowering asyncFinally_;
    bool objectEmission_ = false;
    bool lazyObjectBody_ = false;
    const Declaration* objectOwner_ = nullptr;
    std::size_t objectSequence_ = 0;
    std::map<std::string, std::size_t> objectTypeIds_;
    std::map<std::string, const Method*> objectMethods_;
    std::map<std::string, std::string> objectFunctions_;
    bool usesBudget_ = false;
    bool localAsync_ = false;
    bool internalEffect_ = false;
    bool classParallel_ = false;
    bool classSynchronization_ = false;
    std::size_t lockSequence_ = 0;
    bool workerEmission_ = false;
    std::map<std::string, std::string> workerCaptures_;
    std::size_t parallelSequence_ = 0;
};

nlohmann::json fields(const std::vector<Field>& values, bool visibleOnly) {
    auto result = nlohmann::json::array();
    for (const auto& field : values) {
        if (!visibleOnly || field.visible)
            result.push_back({{"name", field.name}, {"type", field.type.text()}});
    }
    return result;
}

nlohmann::json methods(const Declaration& declaration) {
    auto result = nlohmann::json::array();
    for (const auto& method : declaration.methods) {
        if (method.visible && !method.constructor) {
            result.push_back({{"name", method.name}, {"returnType", method.result.text()},
                              {"parameters", fields(method.parameters, false)},
                              {"pure", method.pure}, {"async", method.asynchronous}});
            if (method.staticMethod)
                result.back()["static"] = true;
            if (!method.sourceName.empty())
                result.back()["sourceName"] = method.sourceName;
        }
    }
    return result;
}

}

std::string cppType(const Type& type, const Program& program) {
    if (type.name == "uint32")
        return "std::uint32_t";
    if (type.name == "int")
        return "std::int64_t";
    if (type.name == "Error")
        return "pipelang::model::Error";
    if (type.name == "float")
        return "double";
    if (type.name == "float32")
        return "float";
    if (type.name == "string")
        return "std::string";
    if (type.name == "ByteBuffer")
        return "pipelang::model::ByteBuffer";
    if (type.name == "CancellationToken")
        return "pipelang::model::CancellationToken";
    if (type.name == "ContextOwner")
        return "std::shared_ptr<pipelang::model::ContextOwner>";
    const auto* object = findType(program, type.name);
    if (program.objectEnabled && object && ((object->kind == "Class" && !object->modelExported) || object->sourceInterface))
        return "pipelang::model::ObjectRef<pl_object_tag_" + object->name + ">";
    if (isSynchronizationHandle(type.name))
        return "pipelang::model::" + type.name;
    if (type.name == "FixedArray")
        return "pipelang::model::FixedArray<" + cppType(type.arguments.front(), program) + ", " +
            std::to_string(type.fixedExtent.value()) + ">";
    if (type.name == "Task" || type.name == "Result" || type.name == "IResult" || type.name == "Optional" || type.name == "List" ||
        type.name == "Atomic" || type.name == "Array" || type.name == "Dictionary" || type.name == "HashSet" ||
        type.name == "Queue" || type.name == "Stack" || type.name == "KeyValuePair" ||
        isConcurrentCollection(type.name)) {
        std::string result = "pipelang::model::" + type.name + "<";
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
            if (index != 0)
                result += ", ";
            result += cppType(type.arguments[index], program);
        }
        return result + ">";
    }
    if (service(program, type))
        return "std::shared_ptr<" + type.name + ">";
    return type.name;
}

std::string generate(const Program& program, const std::string& nameSpace) {
    return Generator(program).run(nameSpace);
}

std::string generateCompositionFactory(const Program& program, const std::string& nameSpace, bool compiled) {
    return Generator(program, compiled).compositionFactory(nameSpace);
}

CompiledProject generateCompiledProject(const Program& program, const std::string& nameSpace) {
    if (!program.projectEnabled)
        throw Diagnostic("compiled artifacts require project semantics", 0);
    Generator generator(program, true);
    auto header = generator.run(nameSpace);
    return {std::move(header), generator.implementationFiles()};
}

std::string generateProjectTypes(const Program& program, const std::string& nameSpace) {
    if (!program.projectEnabled)
        throw Diagnostic("standalone project types require the project protocol", 0);
    return Generator(program).run(nameSpace, true);
}

std::string generateNumericHeader(const Program& program, const std::string& nameSpace) {
    if (!program.projectEnabled)
        throw Diagnostic("numeric headers require project semantics", 0);
    return Generator(program).runNumericHeader(nameSpace);
}

nlohmann::json manifest(const Program& program, const std::string& nameSpace) {
    nlohmann::json result = {{"protocol", "pipelang.native-model-manifest.v1"}, {"namespace", nameSpace},
                             {"records", nlohmann::json::array()}, {"enums", nlohmann::json::array()},
                             {"interfaces", nlohmann::json::array()}, {"classes", nlohmann::json::array()}};
    if (program.objectEnabled) {
        result["protocol"] = "pipelang.native-model-manifest.v2";
        result["objects"] = nlohmann::json::array();
        result["sourceInterfaces"] = nlohmann::json::array();
    }
    for (const auto& declaration : program.declarations) {
        nlohmann::json item;
        std::string category;
        if (declaration.kind == "Record") {
            category = "records";
            item = {{"name", declaration.name}, {"fields", fields(declaration.fields, true)}};
        } else if (declaration.kind == "Enum") {
            category = "enums";
            item = {{"name", declaration.name}, {"values", declaration.values}};
        } else if (declaration.kind == "Interface") {
            category = declaration.sourceInterface ? "sourceInterfaces" : "interfaces";
            item = {{"name", declaration.name}, {"methods", methods(declaration)}};
        } else {
            category = program.objectEnabled && !declaration.modelExported ? "objects" : "classes";
            auto publicFields = fields(declaration.fields, true);
            auto publicMethods = methods(declaration);
            for (const auto& ancestor : declaration.ancestors) {
                const auto& base = *findType(program, ancestor);
                for (const auto& field : fields(base.fields, true))
                    publicFields.push_back(field);
                for (const auto& inherited : methods(base)) {
                    const bool present = std::any_of(publicMethods.begin(), publicMethods.end(), [&](const auto& method) {
                        if (method.at("name") != inherited.at("name") || method.at("parameters").size() != inherited.at("parameters").size())
                            return false;
                        for (std::size_t index = 0; index < method.at("parameters").size(); ++index) {
                            if (method.at("parameters")[index].at("type") != inherited.at("parameters")[index].at("type"))
                                return false;
                        }
                        return true;
                    });
                    if (!present)
                        publicMethods.push_back(inherited);
                }
            }
            auto constructors = nlohmann::json::array();
            for (const auto& method : declaration.methods) {
                if (method.constructor)
                    constructors.push_back({{"parameters", fields(method.parameters, false)}});
            }
            item = {{"name", declaration.name}, {"fields", std::move(publicFields)},
                    {"methods", std::move(publicMethods)}, {"constructor", constructors.at(0)}};
            if (constructors.size() > 1)
                item["constructorOverloads"] = constructors;
            if (category == "objects") {
                item["implements"] = nlohmann::json::array();
                for (const auto& type : declaration.interfaces)
                    item["implements"].push_back(type.name);
            }
        }
        if (!declaration.sourceName.empty())
            item["sourceName"] = declaration.sourceName;
        result[category].push_back(std::move(item));
    }
    const auto exports = constantsManifest(program, nameSpace);
    if (!exports.empty())
        result["constants"] = exports;
    if (program.projectEnabled) {
        if (program.activeHostNumericEnvironment)
            result["nativeNumericEnvironment"] = "active-host";
        auto numericExports = nlohmann::json::array();
        auto observationExports = nlohmann::json::array();
        for (const auto& declaration : program.declarations) {
            for (const auto& method : declaration.methods) {
                NativeNumericExports::Projection numericProjection;
                if (NativeNumericExports::admits(program, declaration, method, true, &numericProjection)) {
                    numericExports.push_back({{"owner", declaration.sourceName}, {"method", method.sourceName.empty() ? method.name : method.sourceName},
                        {"returnType", method.result.text()}, {"parameters", fields(method.parameters, false)},
                        {"allocationFree", true}, {"noexcept", true},
                        {"binary32Environment", numericProjection.requiresBinary32Environment
                            ? (program.activeHostNumericEnvironment ? "active-host" : "caller-admitted") : "not-required"}});
                }
                if (NativeNumericExports::admitsObservation(program, declaration, method)) {
                    observationExports.push_back({{"owner", declaration.sourceName}, {"method", method.sourceName.empty() ? method.name : method.sourceName},
                        {"returnType", method.result.text()}, {"parameters", fields(method.parameters, false)},
                        {"allocationFree", true}, {"noexcept", true}, {"binary32Environment", "not-required"},
                        {"effects", "current-thread-observation"}});
                }
            }
        }
        if (!numericExports.empty())
            result["nativeNumericExports"] = std::move(numericExports);
        if (!observationExports.empty())
            result["nativeObservationExports"] = std::move(observationExports);
    }
    return result;
}

}
