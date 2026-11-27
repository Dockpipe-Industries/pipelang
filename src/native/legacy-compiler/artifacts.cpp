// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <regex>

namespace pipelang::legacy {
namespace {
using Ordered = nlohmann::ordered_json;
const Declaration& entry(const Program& program, const std::string& requested) {
    for (const auto& declaration : program.declarations) {
        if (!declaration.contract && declaration.visible && (requested.empty() || declaration.name == requested))
            return declaration;
    }
    throw std::runtime_error("no matching public legacy class");
}
Value zero(const std::string& type) {
    if (type == "string")
        return {type, ""};
    if (type == "bool")
        return {type, false};
    if (type == "float")
        return {type, 0.0};
    if (type == "int")
        return {type, std::int64_t{0}};
    // Legacy configuration schema fields are placeholders, not v3 objects.
    return {"", nullptr};
}
std::map<std::string, Value> defaults(const Declaration& declaration) {
    std::map<std::string, Value> values;
    for (const auto& field : declaration.fields)
        values[field.name] = field.initial ? evaluate(field.initial, {}) : zero(field.type);
    return values;
}
std::string bindingName(const std::string& name) {
    if (name.find('_') != std::string::npos)
        return name;
    const auto upper = [](char value) { return value >= 'A' && value <= 'Z'; };
    const auto lower = [](char value) { return value >= 'a' && value <= 'z'; };
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t index = 1; index < name.size(); ++index) {
        const auto previous = name[index - 1];
        const bool digit = previous >= '0' && previous <= '9';
        if (upper(name[index]) && (lower(previous) || digit || (upper(previous) && index + 1 < name.size() && lower(name[index + 1])))) {
            parts.push_back(name.substr(start, index - start));
            start = index;
        }
    }
    parts.push_back(name.substr(start));
    const bool terraform = parts.size() >= 2 && (parts[0] == "Tf" || parts[0] == "TF" || parts[0] == "tf") &&
        (parts[1] == "Var" || parts[1] == "VAR" || parts[1] == "var");
    std::string result = terraform ? "TF_VAR" : "";
    for (std::size_t index = terraform ? 2 : 0; index < parts.size(); ++index) {
        if (!result.empty())
            result += '_';
        for (char character : parts[index]) {
            if (terraform && character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
            if (!terraform && character >= 'a' && character <= 'z')
                character = static_cast<char>(character - 'a' + 'A');
            result += character;
        }
    }
    return result;
}
std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char character : value)
        result += character == '\'' ? "'\"'\"'" : std::string(1, character);
    return result + '\'';
}
std::string jsonText(const std::string& value) {
    auto bytes = Json(value).dump();
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == '<')
            result += "\\u003c";
        else if (bytes[index] == '>')
            result += "\\u003e";
        else if (bytes[index] == '&')
            result += "\\u0026";
        else if (bytes.substr(index, 3) == "\xe2\x80\xa8" || bytes.substr(index, 3) == "\xe2\x80\xa9") {
            result += bytes[index + 2] == '\xa8' ? "\\u2028" : "\\u2029";
            index += 2;
        } else
            result += bytes[index];
    }
    return result;
}
std::string jsonNumber(double value) {
    require(std::isfinite(value), "legacy JSON cannot represent nonfinite values");
    const auto magnitude = std::abs(value);
    const auto format = magnitude != 0 && (magnitude < 1e-6 || magnitude >= 1e21)
        ? std::chars_format::scientific : std::chars_format::fixed;
    std::array<char, 1024> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format);
    require(result.ec == std::errc{}, "cannot format legacy JSON number");
    std::string text(buffer.data(), result.ptr);
    const auto exponent = text.find('e');
    if (exponent != std::string::npos) {
        const auto digits = exponent + 2;
        while (text.size() > digits + 1 && text[digits] == '0')
            text.erase(digits, 1);
    }
    return text;
}
std::string json(const Ordered& value, unsigned depth = 0) {
    if (value.is_string())
        return jsonText(value.get<std::string>());
    if (value.is_number_float())
        return jsonNumber(value.get<double>());
    if (!value.is_object() && !value.is_array())
        return value.dump();
    if (value.empty())
        return value.is_object() ? "{}" : "[]";
    const std::string indent((depth + 1) * 2, ' ');
    std::string result = value.is_object() ? "{\n" : "[\n";
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (item != value.begin())
            result += ",\n";
        result += indent;
        if (value.is_object())
            result += jsonText(item.key()) + ": ";
        result += json(item.value(), depth + 1);
    }
    return result + '\n' + std::string(depth * 2, ' ') + (value.is_object() ? '}' : ']');
}
std::string yamlScalar(const std::string& value) {
    static const std::set<std::string> resolved{"", "~", "null", "Null", "NULL", "true", "True", "TRUE", "false", "False", "FALSE",
        ".nan", ".NaN", ".NAN", ".inf", ".Inf", ".INF", "+.inf", "+.Inf", "+.INF", "-.inf", "-.Inf", "-.INF", "<<"};
    static const std::regex number("[+-]?([0-9][0-9_]*|0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|([0-9_]*\\.[0-9_]+|[0-9][0-9_]*\\.?)([eE][+-]?[0-9]+)?)");
    static const std::regex timestamp("[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}([Tt ].*)?");
    bool quoted = resolved.contains(value) || std::regex_match(value, number) || std::regex_match(value, timestamp);
    if (quoted)
        return Json(value).dump();
    if (value.front() == ' ' || value.back() == ' ' || value.find(": ") != std::string::npos || value.find(" #") != std::string::npos ||
        value.find_first_of("\n\r\t") != std::string::npos || std::string("#&*!|>'\"%@`{}[],").find(value.front()) != std::string::npos ||
        value == "-" || value == ":" || value == "?" || value.starts_with("- ") || value.starts_with(": ") || value.starts_with("? ")) {
        std::string result = "'";
        for (char character : value)
            result += character == '\'' ? "''" : std::string(1, character);
        return result + '\'';
    }
    return value;
}
void attributes(std::string& output, const std::vector<Annotation>& annotations) {
    for (const auto& annotation : annotations)
        output += "annotation " + annotation.name + " " + annotation.value.type + " " + annotation.value.string() + '\n';
}
}
Artifacts compile(const Program& program, const std::string& requested) {
    check(program);
    const auto& owner = entry(program, requested);
    const auto values = defaults(owner);
    std::vector<const Field*> fields;
    for (const auto& field : owner.fields) {
        if (field.visible)
            fields.push_back(&field);
    }
    std::sort(fields.begin(), fields.end(), [](const Field* first, const Field* second) { return first->name < second->name; });
    std::vector<const Method*> methods;
    for (const auto& method : owner.methods) {
        if (method.visible)
            methods.push_back(&method);
    }
    std::sort(methods.begin(), methods.end(), [](const Method* first, const Method* second) { return first->name < second->name; });
    Ordered manifest{{"schema", 1}, {"entry_class", owner.name}, {"fields", Ordered::array()}, {"methods", Ordered::array()}};
    Artifacts result;
    result.entry = owner.name;
    auto name = owner.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    result.workflow = "name: " + yamlScalar(name) + "\ndescription: Generated by PipeLang v0.0.0.1\nvars:";
    result.workflow += fields.empty() ? " {}\n" : "\n";
    for (const auto* field : fields) {
        const auto& value = values.at(field->name);
        manifest["fields"].push_back(Ordered{{"name", field->name}, {"type", field->type}, {"value", value.data}});
        result.workflow += "    " + bindingName(field->name) + ": " + yamlScalar(value.string()) + '\n';
        result.environment += "PIPELANG_" + bindingName(field->name) + '=' + shellQuote(value.string()) + '\n';
    }
    for (const auto* method : methods)
        manifest["methods"].push_back(Ordered{{"name", method->name}, {"return_type", method->result}, {"expr", expressionText(method->body)}});
    result.bindings = json(manifest);
    return result;
}
std::string resultJson(const Json& value) {
    return json(Ordered(value));
}
Value invoke(const Program& program, const std::string& ownerName, const std::string& methodName, const std::vector<std::string>& arguments) {
    check(program);
    const auto& owner = entry(program, ownerName);
    const auto method = std::find_if(owner.methods.begin(), owner.methods.end(), [&](const Method& candidate) { return candidate.name == methodName; });
    require(method != owner.methods.end() && method->visible, "unknown or private legacy method");
    require(method->parameters.size() == arguments.size(), "legacy invocation argument count mismatch");
    auto values = defaults(owner);
    for (std::size_t index = 0; index < arguments.size(); ++index)
        values[method->parameters[index].name] = argument(method->parameters[index].type, arguments[index]);
    return evaluate(method->body, values);
}
std::string projection(const Program& program, const std::string& path) {
    std::string output = "file " + path + '\n';
    for (bool contract : {true, false}) {
        for (const auto& declaration : program.declarations) {
            if (declaration.contract != contract)
                continue;
            const std::string visibility = declaration.visible ? "public" : "private";
            output += contract ? "interface " : "class ";
            output += visibility + " " + declaration.name;
            if (!contract)
                output += " : " + declaration.implements;
            output += '\n';
            attributes(output, declaration.annotations);
            for (const auto& field : declaration.fields) {
                output += "field " + std::string(field.visible ? "public" : "private") + " " + field.type + " " + field.name;
                if (!contract)
                    output += " = " + expressionText(field.initial);
                output += '\n';
                attributes(output, field.annotations);
            }
            for (const auto& method : declaration.methods) {
                output += "method " + std::string(method.visible ? "public" : "private") + " " + method.result + " " + method.name;
                for (const auto& parameter : method.parameters)
                    output += " " + parameter.name + ':' + parameter.type;
                if (!contract)
                    output += " => " + expressionText(method.body);
                output += '\n';
                attributes(output, method.annotations);
            }
        }
    }
    return output;
}
}
