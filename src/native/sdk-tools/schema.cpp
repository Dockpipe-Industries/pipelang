// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
#include "../model-compiler/ResourceBudget.hpp"
#include <pipelang/model/limits.hpp>
#include <pipelang/model/collection.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>
#include <limits>

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
void methods(const Json& values, bool identifiers, bool allowStatic) {
    array(values, "methods");
    std::set<std::string> signatures;
    for (const auto& method : values) {
        std::set<std::string> optional{"sourceName"};
        if (allowStatic)
            optional.insert("static");

        exactObject(method, {"name", "returnType", "parameters", "async", "pure"}, optional, "method");
        if (method.contains("static"))
            require(method.at("static").is_boolean(), "method static metadata requires an exact boolean");

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
void nativeValueExports(const Json& manifest, bool observation) {
    const std::string exportKey = observation ? "nativeObservationExports" : "nativeNumericExports";
    const std::string exportKind = observation ? "native observation export" : "native numeric export";
    if (!manifest.contains(exportKey))
        return;
    const auto& exports = manifest.at(exportKey);
    array(exports, exportKind + "s", 32768);
    require(!exports.empty(), exportKind + "s must not be empty");
    const std::set<std::string> builtinNames{
        "void", "bool", "int", "uint32", "float", "float32", "string", "Error", "ByteBuffer",
        "Optional", "Result", "Task", "List", "Array", "FixedArray", "Dictionary", "HashSet", "Queue", "Stack",
        "Atomic", "CancellationToken", "Lock", "SemaphoreSlim", "KeyValuePair",
        "ConcurrentDictionary", "ConcurrentQueue", "ConcurrentStack"
    };
    const auto nominalIdentity = [&](const std::string& name) {
        if (name.empty() || builtinNames.contains(name))
            return false;
        bool atStart = true;
        for (const unsigned char character : name) {
            if (character == '.') {
                if (atStart)
                    return false;
                atStart = true;
                continue;
            }
            const bool letter = (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') || character == '_';
            if (!letter && (atStart || character < '0' || character > '9'))
                return false;
            atStart = false;
        }
        return !atStart;
    };
    struct NominalValue {
        const Json* declaration = nullptr;
        bool enumeration = false;
    };
    std::map<std::string, NominalValue> nominalValues;
    std::set<std::string> ambiguousNames;
    const auto addIdentity = [&](const std::string& name, const Json& declaration, bool enumeration) {
        // Generic authored identities remain available to ordinary model
        // metadata; closed numeric nominal aliases cannot disguise type syntax.
        if (!nominalIdentity(name))
            return;
        const auto [found, inserted] = nominalValues.emplace(name, NominalValue{&declaration, enumeration});
        if (!inserted && found->second.declaration != &declaration)
            ambiguousNames.insert(name);
    };
    for (const auto& category : {"enums", "records"}) {
        for (const auto& declaration : manifest.at(category))
            addIdentity(declaration.at("name").get<std::string>(), declaration, std::string(category) == "enums");
    }
    for (const auto& category : {"enums", "records"}) {
        for (const auto& declaration : manifest.at(category)) {
            if (declaration.contains("sourceName"))
                addIdentity(declaration.at("sourceName").get<std::string>(), declaration, std::string(category) == "enums");
        }
    }
    const auto lookup = [&](const std::string& name) -> const NominalValue* {
        if (ambiguousNames.contains(name))
            return nullptr;
        const auto found = nominalValues.find(name);
        return found == nominalValues.end() ? nullptr : &found->second;
    };
    const auto enumeration = [&](const Json& type) {
        if (!type.is_string())
            return false;
        const auto* found = lookup(type.get<std::string>());
        return found && found->enumeration;
    };
    const auto scalar = [&](const Json& type) {
        if (observation && (type == "float" || type == "float32"))
            return false;
        return type == "int" || type == "bool" || type == "uint32" ||
            (!observation && (type == "float" || type == "float32")) || enumeration(type);
    };
    const auto recordDeclaration = [&](const std::string& name) -> const Json* {
        const auto* found = lookup(name);
        return found && !found->enumeration ? found->declaration : nullptr;
    };
    const auto genericArguments = [](const std::string& value, const std::string& owner)
        -> std::optional<std::vector<std::string>> {
        const auto prefix = owner + "<";
        if (!value.starts_with(prefix) || !value.ends_with('>'))
            return std::nullopt;
        std::vector<std::string> arguments;
        std::size_t start = prefix.size();
        unsigned depth = 0;
        for (std::size_t index = start; index + 1 < value.size(); ++index) {
            const char character = value[index];
            if (character == '<') {
                if (++depth > 12)
                    return std::nullopt;
            } else if (character == '>') {
                if (depth == 0)
                    return std::nullopt;
                --depth;
            } else if (character == ',' && depth == 0) {
                if (index == start)
                    return std::nullopt;
                arguments.push_back(value.substr(start, index - start));
                start = index + 1;
            }
        }
        if (depth != 0 || start + 1 >= value.size())
            return std::nullopt;
        arguments.push_back(value.substr(start, value.size() - start - 1));
        return arguments;
    };
    struct ValueLayout {
        std::size_t nodes = 1;
        unsigned depth = 1;
        std::size_t footprint = 1;
        std::size_t elements = 0;
        std::size_t storageBytes = 8;
        std::size_t ownedBytes = 8;
    };
    constexpr auto maximumElements = pipelang::model::maximumListElements;
    constexpr auto maximumBytes = pipelang::model::maximumListBytes;
    const auto addBounded = [](std::size_t first, std::size_t second, std::size_t maximum)
        -> std::optional<std::size_t> {
        if (first > maximum || second > maximum - first)
            return std::nullopt;
        return first + second;
    };
    const auto multiplyBounded = [](std::size_t first, std::size_t second, std::size_t maximum)
        -> std::optional<std::size_t> {
        if (first != 0 && second > maximum / first)
            return std::nullopt;
        return first * second;
    };
    const auto closedValue = [&](const std::string& name) -> std::optional<ValueLayout> {
        std::set<std::string> activeRecords;
        std::size_t remaining = pipelang::model::defaultEvaluationSteps;
        std::function<std::optional<ValueLayout>(const std::string&)> visit;
        visit = [&](const std::string& current) -> std::optional<ValueLayout> {
            if (remaining == 0 || current.size() > 4096)
                return std::nullopt;
            --remaining;
            if (observation && (current == "float" || current == "float32"))
                return std::nullopt;
            if (scalar(current))
                return ValueLayout{};
            if (const auto arguments = genericArguments(current, "FixedArray")) {
                if (arguments->size() != 2)
                    return std::nullopt;
                const auto& countText = arguments->back();
                if (countText.empty() || (countText.size() > 1 && countText.front() == '0') ||
                    !std::all_of(countText.begin(), countText.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return std::nullopt;
                std::size_t count = 0;
                const auto parsed = std::from_chars(countText.data(), countText.data() + countText.size(), count);
                if (parsed.ec != std::errc{} || parsed.ptr != countText.data() + countText.size() || count > maximumElements)
                    return std::nullopt;
                const auto nested = visit(arguments->front());
                if (!nested)
                    return std::nullopt;
                const auto footprint = multiplyBounded(nested->footprint, count, maximumElements);
                const auto perElement = addBounded(1, nested->elements, maximumElements);
                const auto elements = perElement ? multiplyBounded(*perElement, count, maximumElements) : std::nullopt;
                const auto storage = multiplyBounded(nested->storageBytes, count, maximumBytes);
                const auto payloadBytes = multiplyBounded(nested->ownedBytes, count, maximumBytes);
                if (!footprint || !elements || !storage || !payloadBytes)
                    return std::nullopt;
                const auto owned = addBounded(std::max<std::size_t>(8, *storage), *payloadBytes, maximumBytes);
                if (!owned)
                    return std::nullopt;
                const auto weightedNodes = count == 0 ? std::size_t{1}
                    : (nested->nodes > 256 / count ? std::size_t{257} : 1 + nested->nodes * count);
                return ValueLayout{weightedNodes, nested->depth + 1,
                    std::max<std::size_t>(1, *footprint), *elements, std::max<std::size_t>(8, *storage), *owned};
            }
            if (const auto arguments = genericArguments(current, "Optional")) {
                if (arguments->size() != 1)
                    return std::nullopt;
                const auto nested = visit(arguments->front());
                if (!nested)
                    return std::nullopt;
                const auto footprint = addBounded(nested->footprint, 1, maximumElements);
                const auto storage = addBounded(nested->storageBytes, 8, maximumBytes);
                const auto owned = storage ? addBounded(*storage, nested->ownedBytes, maximumBytes) : std::nullopt;
                if (!footprint || !storage || !owned)
                    return std::nullopt;
                return ValueLayout{nested->nodes + 1, nested->depth + 1, *footprint, nested->elements, *storage, *owned};
            }
            const auto* record = recordDeclaration(current);
            if (!record)
                return std::nullopt;
            const auto identity = record->at("name").get<std::string>();
            if (!activeRecords.insert(identity).second)
                return std::nullopt;
            ValueLayout layout;
            std::size_t footprint = 0;
            std::size_t elements = 0;
            std::size_t storage = 0;
            std::size_t payloadBytes = 0;
            for (const auto& field : record->at("fields")) {
                const auto child = visit(field.at("type").get<std::string>());
                if (!child || child->nodes > 256 || layout.nodes > 256 - child->nodes || child->depth >= 32)
                    return std::nullopt;
                const auto nextFootprint = addBounded(footprint, child->footprint, maximumElements);
                const auto nextElements = addBounded(elements, child->elements, maximumElements);
                const auto nextStorage = addBounded(storage, child->storageBytes, maximumBytes);
                const auto nextBytes = addBounded(payloadBytes, child->ownedBytes, maximumBytes);
                if (!nextFootprint || !nextElements || !nextStorage || !nextBytes)
                    return std::nullopt;
                footprint = *nextFootprint;
                elements = *nextElements;
                storage = *nextStorage;
                payloadBytes = *nextBytes;
                layout.nodes += child->nodes;
                layout.depth = std::max(layout.depth, child->depth + 1);
            }
            activeRecords.erase(identity);
            layout.footprint = std::max<std::size_t>(1, footprint);
            layout.elements = elements;
            layout.storageBytes = std::max<std::size_t>(8, storage);
            const auto owned = addBounded(layout.storageBytes, payloadBytes, maximumBytes);
            if (!owned)
                return std::nullopt;
            layout.ownedBytes = *owned;
            return layout;
        };
        return visit(name);
    };
    const auto numericValue = [&](const Json& type) {
        if (!type.is_string())
            return false;
        const auto name = type.get<std::string>();
        if (closedValue(name))
            return true;
        const auto arguments = genericArguments(name, "Result");
        if (!arguments || arguments->size() != 2)
            return false;
        const auto& payload = arguments->front();
        const auto& error = arguments->back();
        if (!enumeration(error) && !recordDeclaration(error))
            return false;
        const auto payloadLayout = payload == "void" ? std::optional(ValueLayout{0, 0, 0, 0, 0, 0}) : closedValue(payload);
        const auto errorLayout = closedValue(error);
        if (!payloadLayout || !errorLayout)
            return false;
        const auto footprint = addBounded(payloadLayout->footprint, errorLayout->footprint, maximumElements);
        const auto elements = addBounded(payloadLayout->elements, errorLayout->elements, maximumElements);
        const auto storage = addBounded(payloadLayout->storageBytes, errorLayout->storageBytes, maximumBytes);
        const auto taggedStorage = storage ? addBounded(*storage, 24, maximumBytes) : std::nullopt;
        const auto payloadBytes = addBounded(payloadLayout->ownedBytes, errorLayout->ownedBytes, maximumBytes);
        return footprint && *footprint < maximumElements && elements && taggedStorage && payloadBytes &&
            addBounded(*taggedStorage, *payloadBytes, maximumBytes).has_value();
    };
    std::set<std::string> signatures;
    for (const auto& item : exports) {
        std::set<std::string> required{"owner", "method", "returnType", "parameters", "allocationFree", "noexcept", "binary32Environment"};
        if (observation)
            required.insert("effects");
        exactObject(item, required, {}, exportKind);
        const auto owner = text(item.at("owner"), "numeric export owner", 4096);
        const auto name = text(item.at("method"), "numeric export method", 1024);
        identifier(item.at("method"));
        fields(item.at("parameters"), true);
        require(numericValue(item.at("returnType")) && item.at("parameters").size() <= 32,
                "native numeric export signature exceeds numeric value bounds");
        for (const auto& parameter : item.at("parameters"))
            require(numericValue(parameter.at("type")) || (!observation && parameter.at("type") == "string"),
                "native numeric export parameter must be a closed numeric value or borrowed byte text");
        const auto& environment = item.at("binary32Environment");
        const bool admittedEnvironment = environment == "not-required" ||
            (!observation && ((environment == "caller-admitted" && !manifest.contains("nativeNumericEnvironment")) ||
                (environment == "active-host" && manifest.value("nativeNumericEnvironment", "") == "active-host")));
        require(item.at("allocationFree") == true && item.at("noexcept") == true &&
                admittedEnvironment, exportKind + " guarantee mismatch");
        if (observation)
            require(item.at("effects") == "current-thread-observation", "native observation export effect mismatch");
        const auto& classes = manifest.at("classes");
        const auto declaration = std::find_if(classes.begin(), classes.end(), [&](const Json& candidate) {
            return candidate.value("sourceName", "") == owner;
        });
        require(declaration != classes.end() && declaration->at("fields").empty() &&
                declaration->at("constructor").at("parameters").empty() &&
                !declaration->contains("constructorOverloads"), "native numeric export has unknown or stateful owner");
        const auto& methods = declaration->at("methods");
        const auto method = std::find_if(methods.begin(), methods.end(), [&](const Json& candidate) {
            return candidate.value("sourceName", candidate.at("name").get<std::string>()) == name &&
                candidate.at("parameters") == item.at("parameters") && candidate.at("returnType") == item.at("returnType");
        });
        require(method != methods.end() && method->at("pure") == !observation && method->at("async") == false,
                exportKind + " does not match its public synchronous method effect");
        require(signatures.insert(owner + '.' + name + item.at("parameters").dump()).second,
                "duplicate native numeric export");
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
    if (manifest.contains("nativeNumericEnvironment"))
        require(manifest.at("nativeNumericEnvironment") == "active-host", "unknown native numeric environment");
    exactObject(manifest, required, {"constants", "nativeNumericExports", "nativeObservationExports", "nativeNumericEnvironment"}, "model manifest");
    require(manifest.at("protocol") == (objects ? "pipelang.native-model-manifest.v2" : "pipelang.native-model-manifest.v1") &&
            manifest.at("namespace") == nameSpace, "model manifest protocol or namespace mismatch");
    if (objects)
        identifier(manifest.at("namespace"));
    std::set<std::string> declarations;
    std::set<std::string> sourceInterfaces;
    if (objects) {
        array(manifest.at("sourceInterfaces"), "source interfaces", models::resourceBudget().declarations);
        for (const auto& contract : manifest.at("sourceInterfaces"))
            sourceInterfaces.insert(text(contract.at("name"), "source interface name", 1024));
    }
    for (const auto& category : {"records", "enums", "interfaces", "classes", "objects", "sourceInterfaces"}) {
        if (!manifest.contains(category))
            continue;
        const auto& values = manifest.at(category);
        array(values, category, models::resourceBudget().declarations);
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
                methods(declaration.at("methods"), objects, objects && (kind == "classes" || kind == "objects"));
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
    nativeValueExports(manifest, false);
    nativeValueExports(manifest, true);
    if (manifest.contains("constants")) {
        const auto& constants = manifest.at("constants");
        array(constants, "constant exports", 32768);
        require(!constants.empty(), "constant exports must not be empty");
        std::set<std::string> owners;
        for (const auto& category : {"classes", "objects"}) {
            if (!manifest.contains(category))
                continue;
            for (const auto& declaration : manifest.at(category))
                owners.insert(declaration.value("sourceName", declaration.at("name").get<std::string>()));
        }
        std::set<std::string> identities;
        for (const auto& constant : constants) {
            exactObject(constant, {"module", "owner", "name", "type", "encoding", "cpp_name", "value"}, {}, "constant export");
            require(constant.at("module").is_string(), "constant module must be text");
            const auto module = constant.at("module").get<std::string>();
            require(module.size() <= 1024, "constant module exceeds its limit");
            const auto owner = text(constant.at("owner"), "constant owner", 4096);
            const auto name = text(constant.at("name"), "constant name", 1024);
            identifier(constant.at("name"));
            require(owners.contains(owner) && identities.insert(owner + '.' + name).second,
                "constant export has unknown owner or duplicate identity");
            const auto separator = owner.rfind('.');
            const auto className = separator == std::string::npos ? owner : owner.substr(separator + 1);
            identifier(className);
            require(owner == (module.empty() ? className : module + '.' + className), "constant module and owner disagree");
            std::string cppOwner;
            std::size_t begin = 0;
            while (begin < module.size()) {
                const auto end = module.find('.', begin);
                const auto part = module.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                identifier(part);
                cppOwner += part + "::";
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            require(module.empty() || module.back() != '.', "constant module has empty component");
            cppOwner += className;
            require(constant.at("cpp_name") == nameSpace + "::constants::" + cppOwner + "::" + name,
                "constant C++ identity mismatch");
            const auto type = text(constant.at("type"), "constant type", 32);
            const auto value = text(constant.at("value"), "constant value", 32);
            static const std::map<std::string, std::string> encodings{
                {"bool", "boolean"}, {"int", "signed64-decimal"}, {"uint32", "unsigned32-decimal"},
                {"float32", "ieee754-binary32-bits"}, {"float", "ieee754-binary64-bits"}
            };
            require(encodings.contains(type) && constant.at("encoding") == encodings.at(type), "constant type or encoding mismatch");
            if (type == "bool") {
                require(value == "true" || value == "false", "invalid boolean constant");
            } else if (type == "int") {
                std::int64_t parsed = 0;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                require(result.ec == std::errc{} && result.ptr == value.data() + value.size() && std::to_string(parsed) == value,
                    "invalid signed64 constant");
            } else {
                std::uint64_t parsed = 0;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                require(result.ec == std::errc{} && result.ptr == value.data() + value.size() && std::to_string(parsed) == value,
                    "invalid constant bit value");
                if (type == "uint32" || type == "float32")
                    require(parsed <= std::numeric_limits<std::uint32_t>::max(), "constant value exceeds32bits");
            }
        }
    }
}
void requireViewCompatible(const Json& manifest) {
    const auto rejectUnsupportedPrimitive = [&](const auto& self, const Json& value) -> void {
        if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                if ((key == "type" || key == "returnType") && child.is_string()) {
                    const auto type = child.template get<std::string>();
                    for (const std::string primitive : {"float32", "uint32"}) {
                        std::size_t position = 0;
                        while ((position = type.find(primitive, position)) != std::string::npos) {
                            const auto end = position + primitive.size();
                            const bool startsToken = position == 0 || type[position - 1] == '<' || type[position - 1] == ',';
                            const bool endsToken = end == type.size() || type[end] == '>' || type[end] == ',';
                            require(!startsToken || !endsToken, "view bindings do not support " + primitive + " values");
                            position = end;
                        }
                    }
                }
                self(self, child);
            }
        } else if (value.is_array()) {
            for (const auto& child : value)
                self(self, child);
        }
    };
    rejectUnsupportedPrimitive(rejectUnsupportedPrimitive, manifest);
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
