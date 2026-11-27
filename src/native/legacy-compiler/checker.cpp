// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <algorithm>

namespace pipelang::legacy {
namespace {
std::string quoted(const std::string& value) {
    return Json(value).dump();
}
std::string inferred(const Expr& expression, const std::map<std::string, std::string>& types, const std::string& prefix) {
    try {
        return infer(expression, types);
    } catch (const Diagnostic& error) {
        throw Diagnostic(error.code, prefix + error.what(), error.file, error.start, error.end, error.related);
    }
}
void conformance(const Declaration& owner, const Declaration& contract) {
    for (const auto& required : contract.fields) {
        const auto actual = std::find_if(owner.fields.begin(), owner.fields.end(), [&](const Field& field) { return field.name == required.name; });
        if (actual == owner.fields.end())
            throw Diagnostic("PL3008", "class " + owner.name + " missing interface field " + contract.name + '.' + required.name,
                owner.span, {{required.span, "required interface field"}});
        if (!actual->visible)
            throw Diagnostic("PL3008", "class " + owner.name + " field " + required.name + " must be public to satisfy interface " + contract.name,
                actual->span, {{required.span, "required interface field"}});
        if (actual->type != required.type)
            throw Diagnostic("PL3008", "class " + owner.name + " field " + required.name + " type " + actual->type + " does not match interface " + required.type,
                actual->typeSpan, {{required.typeSpan, "interface field type"}});
    }
    for (const auto& required : contract.methods) {
        const auto actual = std::find_if(owner.methods.begin(), owner.methods.end(), [&](const Method& method) { return method.name == required.name; });
        if (actual == owner.methods.end())
            throw Diagnostic("PL3008", "class " + owner.name + " missing interface method " + contract.name + '.' + required.name,
                owner.span, {{required.span, "required interface method"}});
        if (!actual->visible)
            throw Diagnostic("PL3008", "class " + owner.name + " method " + required.name + " must be public to satisfy interface " + contract.name,
                actual->span, {{required.span, "interface method signature"}});
        if (actual->result != required.result)
            throw Diagnostic("PL3008", "class " + owner.name + " method " + required.name + " return type " + actual->result + " does not match interface " + required.result,
                actual->resultSpan, {{required.resultSpan, "interface method signature"}});
        if (actual->parameters.size() != required.parameters.size())
            throw Diagnostic("PL3008", "class " + owner.name + " method " + required.name + " parameter count mismatch",
                actual->span, {{required.span, "interface method signature"}});
        for (std::size_t index = 0; index < required.parameters.size(); ++index) {
            if (actual->parameters[index].type != required.parameters[index].type)
                throw Diagnostic("PL3008", "class " + owner.name + " method " + required.name + " parameter " + std::to_string(index + 1) +
                    " type " + actual->parameters[index].type + " does not match interface " + required.parameters[index].type,
                    actual->parameters[index].typeSpan, {{required.parameters[index].typeSpan, "interface parameter type"}});
        }
    }
}
}
void check(const Program& program) {
    std::map<std::string, const Declaration*> declarations;
    for (const auto& declaration : program.declarations) {
        const auto previous = declarations.find(declaration.name);
        if (previous != declarations.end())
            throw Diagnostic("PL3003", "duplicate " + std::string(declaration.contract ? "interface " : "class ") + quoted(declaration.name),
                declaration.span, {{previous->second->span, "first declaration"}});
        declarations.emplace(declaration.name, &declaration);
    }
    const auto type = [&](const std::string& name, const SourceSpan& span, const std::string& context, const SourceSpan& declaration, const std::string& label) {
        if (name != "string" && name != "int" && name != "float" && name != "bool" && !declarations.contains(name))
            throw Diagnostic("PL3006", context + "unknown type " + quoted(name), span, {{declaration, label}});
    };
    for (const auto& owner : program.declarations) {
        const auto ownerName = std::string(owner.contract ? "interface " : "class ") + owner.name;
        std::map<std::string, SourceSpan> members;
        std::map<std::string, std::string> types;
        const auto member = [&](const std::string& name, const SourceSpan& span) {
            const auto previous = members.find(name);
            if (previous != members.end())
                throw Diagnostic("PL3005", ownerName + " has duplicate member " + quoted(name), span, {{previous->second, "first member"}});
            members.emplace(name, span);
        };
        for (const auto& field : owner.fields) {
            if (owner.contract && !field.visible)
                throw Diagnostic("PL3004", ownerName + " field " + field.name + " must be public", field.span);
            type(field.type, field.typeSpan, ownerName + " field " + field.name + ": ", field.span, "field declaration");
            member(field.name, field.span);
            types[field.name] = field.type;
        }
        for (const auto& method : owner.methods) {
            if (owner.contract && !method.visible)
                throw Diagnostic("PL3004", ownerName + " method " + method.name + " must be public", method.span);
            type(method.result, method.resultSpan, ownerName + " method " + method.name + " return: ", method.span, "method declaration");
            member(method.name, method.span);
            std::map<std::string, SourceSpan> parameters;
            for (const auto& parameter : method.parameters) {
                type(parameter.type, parameter.typeSpan, owner.name + " method " + method.name + " parameter " + parameter.name + ": ", parameter.span, "parameter declaration");
                const auto previous = parameters.find(parameter.name);
                if (previous != parameters.end())
                    throw Diagnostic("PL3005", owner.name + " method " + method.name + " has duplicate parameter " + quoted(parameter.name),
                        parameter.span, {{previous->second, "first parameter"}});
                parameters.emplace(parameter.name, parameter.span);
            }
        }
        if (owner.contract)
            continue;
        for (const auto& field : owner.fields) {
            if (!field.initial)
                continue;
            const auto actual = inferred(field.initial, {}, ownerName + " field " + field.name + " default: ");
            if (actual != field.type)
                throw Diagnostic("PL3009", ownerName + " field " + field.name + " default type " + actual + " does not match " + field.type,
                    field.initial->file, field.initial->start, field.initial->end, {{field.typeSpan, "declared field type"}});
        }
        for (const auto& method : owner.methods) {
            auto local = types;
            for (const auto& parameter : method.parameters)
                local[parameter.name] = parameter.type;
            const auto actual = inferred(method.body, local, ownerName + " method " + method.name + ": ");
            if (actual != method.result)
                throw Diagnostic("PL3009", ownerName + " method " + method.name + " returns " + actual + " but declared " + method.result,
                    method.body->file, method.body->start, method.body->end, {{method.resultSpan, "declared return type"}});
        }
    }
    if (std::none_of(program.declarations.begin(), program.declarations.end(), [](const Declaration& declaration) { return !declaration.contract; }))
        throw Diagnostic("PL3010", "no class declarations found", program.span);
    for (const auto& owner : program.declarations) {
        if (owner.contract || owner.implements.empty() || owner.implements == "IComparable")
            continue;
        const auto found = declarations.find(owner.implements);
        if (found == declarations.end())
            throw Diagnostic("PL3007", "class " + owner.name + " implements unknown interface " + quoted(owner.implements),
                owner.implementsSpan, {{owner.span, "implementing class"}});
        if (!found->second->contract)
            throw Diagnostic("PL3007", "class " + owner.name + " cannot implement non-interface " + quoted(owner.implements),
                owner.implementsSpan, {{found->second->span, "resolved class declaration"}});
        conformance(owner, *found->second);
    }
}
}
