// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
#include <algorithm>
#include <cctype>

namespace pipelang::tools {
namespace {
void array(const Json& value, const std::string& label, std::size_t maximum = 128) {
    require(value.is_array() && value.size() <= maximum, label + " must be a bounded array");
}
void identifier(const Json& value) {
    const auto name = text(value, "schema identifier", 1024);
    require((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_', "invalid emitted identifier");
    require(std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_';
    }), "invalid emitted identifier");
}
void fields(const Json& values, bool identifiers) {
    array(values, "typed fields");
    std::set<std::string> names;
    for (const auto& field : values) {
        exactObject(field, {"name", "type"}, {}, "typed field");
        require(names.insert(text(field.at("name"), "field name", 1024)).second, "duplicate field name");
        text(field.at("type"), "field type", 4096);
        if (identifiers)
            identifier(field.at("name"));
    }
}
void methods(const Json& values, bool identifiers) {
    array(values, "methods");
    std::set<std::string> signatures;
    for (const auto& method : values) {
        exactObject(method, {"name", "returnType", "parameters", "async", "pure"}, {"sourceName"}, "method");
        const auto name = text(method.at("name"), "method name", 1024);
        text(method.at("returnType"), "method return type", 4096);
        fields(method.at("parameters"), identifiers);
        Json types = Json::array();
        for (const auto& parameter : method.at("parameters"))
            types.push_back(parameter.at("type"));
        require(signatures.insert(name + types.dump()).second, "duplicate method signature");
        require(method.at("async").is_boolean() && method.at("pure").is_boolean(), "method effects require exact booleans");
        if (method.contains("sourceName"))
            text(method.at("sourceName"), "authored method identity", 4096);
        if (identifiers)
            identifier(method.at("name"));
    }
}
void constructors(const Json& owner, bool identifiers) {
    const auto check = [&](const Json& constructor) {
        exactObject(constructor, {"parameters"}, {}, "constructor");
        fields(constructor.at("parameters"), identifiers);
    };
    check(owner.at("constructor"));
    if (!owner.contains("constructorOverloads"))
        return;
    const auto& overloads = owner.at("constructorOverloads");
    array(overloads, "constructor overloads", 256);
    require(overloads.size() >= 2 && overloads.front() == owner.at("constructor"), "invalid constructor overload inventory");
    std::set<std::string> signatures;
    for (const auto& constructor : overloads) {
        check(constructor);
        require(constructor.at("parameters").size() <= 32, "constructor parameter bound exceeded");
        Json types = Json::array();
        for (const auto& parameter : constructor.at("parameters"))
            types.push_back(parameter.at("type"));
        require(signatures.insert(types.dump()).second, "duplicate constructor overload");
    }
}
}

void admitModel(const Json& manifest, const std::string& nameSpace) {
    const bool objects = manifest.is_object() && manifest.value("protocol", "") == "pipelang.native-model-manifest.v2";
    std::set<std::string> required{"protocol", "namespace", "records", "enums", "interfaces", "classes"};
    if (objects) {
        required.insert("objects");
        required.insert("sourceInterfaces");
    }
    exactObject(manifest, required, {}, "model manifest");
    require(manifest.at("protocol") == (objects ? "pipelang.native-model-manifest.v2" : "pipelang.native-model-manifest.v1") &&
            manifest.at("namespace") == nameSpace, "model manifest protocol or namespace mismatch");
    if (objects)
        identifier(manifest.at("namespace"));
    std::set<std::string> declarations;
    std::set<std::string> sourceInterfaces;
    if (objects) {
        array(manifest.at("sourceInterfaces"), "source interfaces");
        for (const auto& contract : manifest.at("sourceInterfaces"))
            sourceInterfaces.insert(text(contract.at("name"), "source interface name", 1024));
    }
    for (const auto& category : {"records", "enums", "interfaces", "classes", "objects", "sourceInterfaces"}) {
        if (!manifest.contains(category))
            continue;
        const auto& values = manifest.at(category);
        array(values, category);
        std::set<std::string> names;
        for (const auto& declaration : values) {
            const std::string kind = category;
            std::set<std::string> fieldsRequired{"name"};
            std::set<std::string> optional{"sourceName"};
            if (kind == "records")
                fieldsRequired.insert("fields");
            else if (kind == "enums")
                fieldsRequired.insert("values");
            else if (kind == "interfaces" || kind == "sourceInterfaces")
                fieldsRequired.insert("methods");
            else {
                fieldsRequired.insert({"fields", "methods", "constructor"});
                optional.insert("constructorOverloads");
                if (kind == "objects")
                    fieldsRequired.insert("implements");
            }
            exactObject(declaration, fieldsRequired, optional, "model declaration");
            const auto name = text(declaration.at("name"), "declaration name", 1024);
            require(names.insert(name).second, "duplicate declaration name in category");
            if (objects) {
                identifier(declaration.at("name"));
                require(declarations.insert(name).second, "duplicate object model declaration name");
            }
            if (declaration.contains("sourceName"))
                text(declaration.at("sourceName"), "authored declaration identity", 4096);
            if (declaration.contains("fields"))
                fields(declaration.at("fields"), objects);
            if (declaration.contains("methods"))
                methods(declaration.at("methods"), objects);
            if (declaration.contains("constructor"))
                constructors(declaration, objects);
            if (kind == "enums") {
                array(declaration.at("values"), "enum values");
                std::set<std::string> members;
                for (const auto& value : declaration.at("values")) {
                    require(members.insert(text(value, "enum member", 1024)).second, "duplicate enum member");
                    if (objects)
                        identifier(value);
                }
            }
            if (kind == "objects") {
                array(declaration.at("implements"), "implemented interfaces");
                std::set<std::string> implemented;
                for (const auto& contract : declaration.at("implements")) {
                    identifier(contract);
                    const auto identity = contract.get<std::string>();
                    require(implemented.insert(identity).second && sourceInterfaces.contains(identity), "duplicate or unknown implemented interface");
                }
            }
        }
    }
}
void requireViewCompatible(const Json& manifest) {
    const auto rejectBinary32 = [&](const auto& self, const Json& value) -> void {
        if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                if ((key == "type" || key == "returnType") && child.is_string()) {
                    const auto type = child.template get<std::string>();
                    std::size_t position = 0;
                    while ((position = type.find("float32", position)) != std::string::npos) {
                        const auto end = position + 7;
                        const bool startsToken = position == 0 || type[position - 1] == '<' || type[position - 1] == ',';
                        const bool endsToken = end == type.size() || type[end] == '>' || type[end] == ',';
                        require(!startsToken || !endsToken, "view bindings do not support float32 values");
                        position = end;
                    }
                }
                self(self, child);
            }
        } else if (value.is_array()) {
            for (const auto& child : value)
                self(self, child);
        }
    };
    rejectBinary32(rejectBinary32, manifest);
    require(manifest.at("protocol") != "pipelang.native-model-manifest.v2", "view bindings do not support object model manifest v2");
    for (const auto& category : {"records", "enums", "interfaces", "classes"}) {
        for (const auto& declaration : manifest.at(category)) {
            require(!declaration.contains("sourceName") && !declaration.contains("constructorOverloads"),
                    "view bindings do not support modules, specializations or overloaded constructors");
            if (!declaration.contains("methods"))
                continue;
            std::set<std::string> names;
            for (const auto& method : declaration.at("methods")) {
                require(!method.contains("sourceName") && names.insert(method.at("name").get<std::string>()).second,
                        "view bindings do not support specialized or overloaded methods");
            }
        }
    }
}
}
