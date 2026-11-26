#include "core_json.hpp"
#include "sha256.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace pipelang::native {
namespace {

Json canonicalSemanticType(const Json& type) {
    Json result = Json::object();
    result["kind"] = textMember(type, "kind");
    for (const auto* name : {"primitive", "package_id", "path", "name"}) {
        if (type.contains(name) && !type.at(name).is_null()) {
            const auto value = textMember(type, name);
            if (!value.empty())
                result[name] = value;
        }
    }
    if (type.contains("arguments") && !type.at("arguments").is_null()
        && !type.at("arguments").empty()) {
        result["arguments"] = Json::array();
        for (const auto& argument : type.at("arguments"))
            result["arguments"].push_back(canonicalSemanticType(argument));
    }
    return result;
}

std::string goJson(const Json& value) {
    // encoding/json escapes HTML and the JavaScript line separators even when
    // these characters occur in identity metadata used for record hashing.
    const auto input = value.dump();
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto character = static_cast<unsigned char>(input[index]);
        if (character == '<') {
            output += "\\u003c";
        } else if (character == '>') {
            output += "\\u003e";
        } else if (character == '&') {
            output += "\\u0026";
        } else if (index + 2 < input.size() && character == 0xe2
            && static_cast<unsigned char>(input[index + 1]) == 0x80
            && (static_cast<unsigned char>(input[index + 2]) == 0xa8
                || static_cast<unsigned char>(input[index + 2]) == 0xa9)) {
            output += static_cast<unsigned char>(input[index + 2]) == 0xa8
                ? "\\u2028" : "\\u2029";
            index += 2;
        } else {
            output += input[index];
        }
    }
    return output;
}

}

const Json& member(const Json& object, const std::string& name) {
    if (!object.is_object() || !object.contains(name) || object.at(name).is_null())
        throw std::runtime_error("missing member: " + name);
    return object.at(name);
}

std::string textMember(const Json& object, const std::string& name) {
    const auto& value = member(object, name);
    if (!value.is_string())
        throw std::runtime_error("expected string: " + name);
    return value.get<std::string>();
}

int positionMember(const Json& object, const std::string& name) {
    const auto& value = member(object, name);
    if (!value.is_number_integer())
        throw std::runtime_error("expected integer: " + name);
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("position out of range");
    const auto position = value.get<std::int64_t>();
    if (position < 0 || position > std::numeric_limits<int>::max())
        throw std::runtime_error("position out of range");
    return static_cast<int>(position);
}

std::string identityKey(const Json& identity) {
    const auto package = textMember(identity, "package_id");
    const auto path = textMember(identity, "path");
    if (package.empty() || path.empty())
        throw std::runtime_error("noncanonical function identity");
    return package + '\0' + path;
}

Json canonicalIdentity(const Json& identity) {
    Json result = Json::object();
    result["package_id"] = textMember(identity, "package_id");
    result["path"] = textMember(identity, "path");
    if (identity.contains("callable") && !identity.at("callable").is_null()) {
        const auto& callable = identity.at("callable");
        Json normalized = Json::object();
        const auto& parameters = callable.at("parameters");
        normalized["parameters"] = parameters.is_null() ? Json(nullptr) : Json::array();
        if (!parameters.is_null()) {
            for (const auto& parameter : parameters)
                normalized["parameters"].push_back(canonicalSemanticType(parameter));
        }
        normalized["returns"] = canonicalSemanticType(member(callable, "returns"));
        result["callable"] = std::move(normalized);
    }
    return result;
}

Json canonicalType(const Json& type) {
    Json result = Json::object();
    result["kind"] = textMember(type, "kind");
    if (type.contains("primitive") && !type.at("primitive").is_null()
        && !textMember(type, "primitive").empty())
        result["primitive"] = type.at("primitive");
    if (type.contains("numeric") && !type.at("numeric").is_null()) {
        const auto& numeric = type.at("numeric");
        Json normalized = Json::object();
        normalized["representation"] = textMember(numeric, "representation");
        normalized["bits"] = member(numeric, "bits");
        if (numeric.value("signed", false))
            normalized["signed"] = true;
        result["numeric"] = std::move(normalized);
    }
    for (const auto* name : {"result", "optional", "list"}) {
        if (!type.contains(name) || type.at(name).is_null())
            continue;
        Json normalized = Json::object();
        std::vector<std::string> fields;
        if (std::string(name) == "result")
            fields = {"success", "failure"};
        else if (std::string(name) == "optional")
            fields = {"value"};
        else
            fields = {"element"};
        for (const auto& field : fields)
            normalized[field] = canonicalType(member(type.at(name), field));
        result[name] = std::move(normalized);
    }
    if (type.contains("record") && !type.at("record").is_null()) {
        Json normalized = Json::object();
        const auto& fields = type.at("record").at("fields");
        normalized["fields"] = fields.is_null() ? Json(nullptr) : Json::array();
        if (!fields.is_null()) {
            for (const auto& field : fields) {
                Json normalizedField = Json::object();
                normalizedField["identity"] = canonicalIdentity(member(field, "identity"));
                normalizedField["name"] = textMember(field, "name");
                normalizedField["type"] = canonicalType(member(field, "type"));
                normalized["fields"].push_back(std::move(normalizedField));
            }
        }
        result["record"] = std::move(normalized);
    }
    if (type.contains("enum") && !type.at("enum").is_null())
        throw std::runtime_error("unsupported enum type metadata");
    if (type.contains("identity") && !type.at("identity").is_null())
        result["identity"] = canonicalIdentity(type.at("identity"));
    if (type.contains("name") && !type.at("name").is_null() && !textMember(type, "name").empty())
        result["name"] = type.at("name");
    if (type.contains("arguments") && !type.at("arguments").is_null() && !type.at("arguments").empty()) {
        result["arguments"] = Json::array();
        for (const auto& argument : type.at("arguments"))
            result["arguments"].push_back(canonicalType(argument));
    }
    return result;
}

std::string typeKey(const Json& type) {
    return goJson(canonicalType(type));
}

std::string typeName(const Json& type) {
    const auto kind = textMember(type, "kind");
    if (kind == "primitive") {
        const auto primitive = textMember(type, "primitive");
        if (primitive == "bool")
            return "bool";
        if (primitive == "string")
            return "std::string";
    } else if (kind == "numeric") {
        const auto& numeric = member(type, "numeric");
        if (textMember(numeric, "representation") == "integer"
            && member(numeric, "bits") == 64 && member(numeric, "signed") == true)
            return "std::int64_t";
    } else if (kind == "arithmetic_error") {
        return "Error";
    } else if (kind == "record") {
        const auto& fields = member(type, "record").at("fields");
        if (!fields.is_array() && !fields.is_null())
            throw std::runtime_error("expected record fields");
        for (const auto& field : fields) {
            const auto& fieldType = member(field, "type");
            const auto fieldKind = textMember(fieldType, "kind");
            if (fieldKind != "primitive" && fieldKind != "numeric")
                throw std::runtime_error("pilot requires flat primitive record fields");
            typeName(fieldType);
        }
        return symbolName("Record_", typeKey(type));
    } else if (kind == "list") {
        const auto& element = member(member(type, "list"), "element");
        if (textMember(element, "kind") == "record")
            return "std::vector<" + typeName(element) + ">";
    } else if (kind == "optional") {
        const auto& value = member(member(type, "optional"), "value");
        if (textMember(value, "kind") == "record")
            return "Optional<" + typeName(value) + ">";
    } else if (kind == "result") {
        const auto& result = member(type, "result");
        const auto& success = member(result, "success");
        const auto& failure = member(result, "failure");
        if (textMember(failure, "kind") == "arithmetic_error"
            && textMember(success, "kind") == "numeric") {
            typeName(failure);
            return "Result<" + typeName(success) + ">";
        }
    }
    throw std::runtime_error("unsupported C++ pilot type " + kind);
}

Json parseEmitterEnvelope(const std::string& input) {
    std::vector<std::set<std::string>> objectKeys;
    std::size_t nodes = 0;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 512 || ++nodes > 100000)
            throw std::runtime_error("pilot Core graph limit");
        if (event == Json::parse_event_t::object_start) {
            objectKeys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            objectKeys.pop_back();
        } else if (event == Json::parse_event_t::key) {
            if (objectKeys.empty() || !objectKeys.back().insert(value.get<std::string>()).second)
                throw std::runtime_error("duplicate JSON key");
        }
        return true;
    };
    return Json::parse(input, callback);
}

Json pilotProgram(const Json& request) {
    if (textMember(request, "contract") != "pipelang.cpp-emitter.request.v1")
        throw std::runtime_error("unsupported emitter request contract");
    return member(request, "program");
}

Json parseRequest(const std::string& input) {
    return pilotProgram(parseEmitterEnvelope(input));
}

}
