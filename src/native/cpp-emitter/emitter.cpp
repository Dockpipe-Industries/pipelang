#include "emitter.hpp"
#include "runtime_support.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pipelang::native {
namespace {

std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0)
            result += ',';
        result += values[index];
    }
    return result;
}

std::string cppString(const std::string& value) {
    std::ostringstream result;
    result << "std::string(\"" << std::oct << std::setfill('0');
    for (const auto character : value)
        result << '\\' << std::setw(3) << static_cast<unsigned>(static_cast<unsigned char>(character));
    result << "\", " << std::dec << value.size() << ')';
    return result.str();
}

std::string integerLiteral(const Json& value) {
    if (!value.is_number_integer())
        throw std::runtime_error("expected integer literal");
    if (value.is_number_unsigned()
        && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("integer literal out of range");
    const auto integer = value.get<std::int64_t>();
    if (integer == std::numeric_limits<std::int64_t>::min())
        return "std::numeric_limits<std::int64_t>::min()";
    return "std::int64_t(" + std::to_string(integer) + "LL)";
}

const Json& arrayMember(const Json& object, const std::string& name) {
    // Go nil slices marshal as null and iterate exactly like empty slices.
    if (!object.is_object() || !object.contains(name))
        throw std::runtime_error("missing member: " + name);
    const auto& value = object.at(name);
    if (!value.is_array() && !value.is_null())
        throw std::runtime_error("expected array: " + name);
    return value;
}

}

void Emitter::append(const std::string& text) {
    if (text.size() > byteLimit - source_.size())
        throw std::runtime_error("generated source limit");
    source_ += text;
}

std::string Emitter::temporary(const std::string& type, const std::string& expression) {
    const auto name = "v" + std::to_string(++temporaryCount_);
    append(type + " " + name + " = " + expression + ";\n");
    return name;
}

void Emitter::collectTypes(const Json& value) {
    if (value.is_array()) {
        for (const auto& element : value)
            collectTypes(element);
        return;
    }
    if (!value.is_object())
        return;
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (field.key() == "type" || field.key() == "return_type" || field.key() == "carrier") {
            typeName(field.value());
            const auto collectType = [&](const auto& self, const Json& type) -> void {
                typeName(type);
                if (textMember(type, "kind") == "record")
                    records_[typeKey(type)] = type;
                for (auto part = type.begin(); part != type.end(); ++part) {
                    if (part.key() == "result") {
                        self(self, member(part.value(), "success"));
                        self(self, member(part.value(), "failure"));
                    } else if (part.key() == "optional") {
                        self(self, member(part.value(), "value"));
                    } else if (part.key() == "list") {
                        self(self, member(part.value(), "element"));
                    } else if (part.key() == "record") {
                        for (const auto& recordField : arrayMember(part.value(), "fields"))
                            self(self, member(recordField, "type"));
                    } else if (part.key() == "arguments") {
                        for (const auto& argument : part.value())
                            self(self, argument);
                    }
                }
            };
            collectType(collectType, field.value());
        } else {
            collectTypes(field.value());
        }
    }
}

void Emitter::emitRecords() {
    for (const auto& entry : records_) {
        const auto& type = entry.second;
        const auto name = typeName(type);
        const auto& fields = arrayMember(member(type, "record"), "fields");
        append("struct " + name + " {\n");
        for (std::size_t index = 0; index < fields.size(); ++index)
            append(typeName(member(fields[index], "type")) + " f" + std::to_string(index) + "{};\n");
        append("};\n");
        append("inline bool operator==(const " + name + "& a,const " + name + "& b){return true");
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto field = "f" + std::to_string(index);
            append(" && a." + field + "==b." + field);
        }
        append(";}\n");
        append("inline bool operator!=(const " + name + "& a,const " + name + "& b){return !(a==b);}\n");
        append("inline void valid(const " + name + "& x){");
        for (std::size_t index = 0; index < fields.size(); ++index)
            append("valid(x.f" + std::to_string(index) + ");");
        append("}\n");
    }
}

std::string Emitter::declaration(const Json& function) const {
    const auto name = functions_.at(identityKey(member(function, "identity")));
    std::vector<std::string> arguments;
    const auto& parameters = arrayMember(function, "parameters");
    for (std::size_t index = 0; index < parameters.size(); ++index)
        arguments.push_back(typeName(member(parameters[index], "type")) + " p" + std::to_string(index));
    return typeName(member(function, "return_type")) + " " + name + "(" + join(arguments) + ")";
}

Json Emitter::generate(const Json& program) {
    const auto contract = textMember(program, "language_contract");
    if (contract != "v0.113.0" && contract != "v0.12.0" && contract != "v0.17.0"
        && contract != "v0.20.0" && contract != "v0.22.0")
        throw std::runtime_error("unsupported pilot Core contract");
    if (textMember(program, "compiler_contract") != "pipelang.compiler.v1")
        throw std::runtime_error("unsupported pilot compiler contract");
    const auto& inputFunctions = arrayMember(program, "functions");
    if (inputFunctions.empty())
        throw std::runtime_error("empty pilot program");
    std::vector<const Json*> functions;
    for (const auto& function : inputFunctions) {
        const auto key = identityKey(member(function, "identity"));
        if (!functions_.emplace(key, symbolName("fn_", key)).second)
            throw std::runtime_error("duplicate function identity");
        const auto& parameters = arrayMember(function, "parameters");
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (positionMember(parameters[index], "position") != static_cast<int>(index))
                throw std::runtime_error("noncanonical parameter position");
        }
        functions.push_back(&function);
    }
    std::sort(functions.begin(), functions.end(), [](const Json* left, const Json* right) {
        return identityKey(member(*left, "identity")) < identityKey(member(*right, "identity"));
    });
    append("// Generated by pipelang.cpp.pilot.v1. C++17 / GCC overflow intrinsics.\n");
    append(runtimeSupport);
    append("\nnamespace pipelang {\n");
    collectTypes(program);
    emitRecords();
    Json bindings = Json::array();
    for (const auto* function : functions) {
        Json binding = Json::object();
        binding["identity"] = canonicalIdentity(member(*function, "identity"));
        binding["name"] = functions_.at(identityKey(member(*function, "identity")));
        bindings.push_back(std::move(binding));
        append("inline " + declaration(*function) + ";\n");
    }
    for (const auto* function : functions) {
        returnType_ = typeName(member(*function, "return_type"));
        append("inline " + declaration(*function) + " {\n");
        Scope scope;
        const auto& parameters = arrayMember(*function, "parameters");
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            const auto name = "p" + std::to_string(index);
            scope.emplace(static_cast<int>(index), name);
            append("valid(" + name + ");\n");
        }
        const auto value = expression(member(*function, "body"), scope);
        append("return " + value + ";\n}\n");
    }
    append("}\n");
    Json response = Json::object();
    response["contract"] = "pipelang.cpp-emitter.response.v1";
    response["source"] = source_;
    response["functions"] = std::move(bindings);
    return response;
}

std::string Emitter::expression(const Json& node, const Scope& scope) {
    const auto& type = member(node, "type");
    const auto cppType = typeName(type);
    const auto kind = textMember(node, "kind");
    const auto emit = [&](const std::string& value) { return temporary("auto", value); };
    const auto sub = [&](const Json& value) { return expression(value, scope); };

    if (kind == "reference") {
        const auto position = positionMember(node, "parameter");
        const auto found = scope.find(position);
        if (found == scope.end())
            throw std::runtime_error("missing reference");
        return found->second;
    }
    if (kind == "literal") {
        const auto& literal = member(node, "literal");
        const auto typeKind = textMember(type, "kind");
        if (typeKind == "arithmetic_error") {
            const auto value = textMember(literal, "string");
            if (value != "overflow" && value != "division_by_zero")
                throw std::runtime_error("bad error literal");
            return emit("Error::" + value);
        }
        if (typeKind == "numeric")
            return emit(integerLiteral(member(literal, "int")));
        if (textMember(type, "primitive") == "string")
            return emit(cppString(textMember(literal, "string")));
        const auto& value = member(literal, "bool");
        if (!value.is_boolean())
            throw std::runtime_error("expected bool literal");
        return emit(value.get<bool>() ? "true" : "false");
    }
    if (kind == "unary") {
        const auto& unary = member(node, "unary");
        const auto value = sub(member(unary, "operand"));
        const auto operation = textMember(unary, "operator");
        if (operation == "not")
            return emit("!" + value);
        if (operation == "negate")
            return emit("negate(" + value + ")");
        throw std::runtime_error("unsupported unary operator");
    }
    if (kind == "binary") {
        const auto& binary = member(node, "binary");
        const auto& leftNode = member(binary, "left");
        const auto left = sub(leftNode);
        const auto operation = textMember(binary, "operator");
        if (operation == "and" || operation == "or") {
            const auto value = temporary("bool", left);
            const auto condition = operation == "or" ? "!" + value : value;
            append("if(" + condition + "){\n");
            const auto right = sub(member(binary, "right"));
            append(value + "=" + right + ";\n}\n");
            return value;
        }
        const auto right = sub(member(binary, "right"));
        if (operation == "add" || operation == "subtract" || operation == "multiply")
            return emit(operation + "(" + left + "," + right + ")");
        static const std::map<std::string, std::string> operators = {
            {"equal", "=="}, {"not_equal", "!="}, {"less_than", "<"},
            {"less_or_equal", "<="}, {"greater_than", ">"}, {"greater_or_equal", ">="}
        };
        const auto found = operators.find(operation);
        if (found == operators.end())
            throw std::runtime_error("unsupported binary operator");
        const auto& leftType = member(leftNode, "type");
        if (leftType.value("primitive", std::string{}) == "string")
            return emit("compare(" + left + "," + right + ")" + found->second + "0");
        return emit(left + found->second + right);
    }
    if (kind == "conditional") {
        const auto& conditional = member(node, "conditional");
        const auto condition = sub(member(conditional, "condition"));
        const auto value = temporary(cppType, "{}");
        append("if(" + condition + "){\n");
        const auto whenTrue = sub(member(conditional, "when_true"));
        append(value + "=" + whenTrue + ";\n}else{\n");
        const auto whenFalse = sub(member(conditional, "when_false"));
        append(value + "=" + whenFalse + ";\n}\n");
        return value;
    }
    if (kind == "immutable_local") {
        const auto& local = member(node, "immutable_local");
        const auto initial = sub(member(local, "initializer"));
        auto nested = scope;
        nested[positionMember(local, "position")] = temporary("auto", initial);
        return expression(member(local, "return"), nested);
    }
    if (kind == "call") {
        const auto& call = member(node, "call");
        std::vector<std::string> arguments;
        for (const auto& argument : arrayMember(call, "arguments"))
            arguments.push_back(sub(argument));
        const auto found = functions_.find(identityKey(member(call, "target")));
        if (found == functions_.end())
            throw std::runtime_error("unknown call");
        return emit(found->second + "(" + join(arguments) + ")");
    }
    if (kind == "field_projection") {
        const auto& field = member(node, "field");
        const auto value = sub(member(field, "receiver"));
        const auto position = positionMember(field, "position");
        const auto& fields = arrayMember(member(member(member(field, "receiver"), "type"), "record"), "fields");
        if (static_cast<std::size_t>(position) >= fields.size())
            throw std::runtime_error("field position out of range");
        return emit(value + ".f" + std::to_string(position));
    }
    if (kind == "record_construct") {
        const auto& record = member(node, "record");
        std::vector<std::string> arguments;
        for (const auto& field : arrayMember(record, "fields"))
            arguments.push_back(sub(member(field, "value")));
        return emit(cppType + "{" + join(arguments) + "}");
    }
    if (kind == "list_empty" || kind == "optional_none") {
        member(node, kind == "list_empty" ? "list_empty" : "none");
        return emit(cppType + "{}");
    }
    if (kind == "list_singleton") {
        const auto value = sub(member(member(node, "list_singleton"), "value"));
        return emit(cppType + "{" + value + "}");
    }
    if (kind == "list_count") {
        const auto value = sub(member(member(node, "list_count"), "value"));
        return emit("static_cast<std::int64_t>(" + value + ".size())");
    }
    if (kind == "list_append" || kind == "list_at") {
        const auto& operation = member(node, kind);
        const auto values = sub(member(operation, "values"));
        const auto value = sub(member(operation, kind == "list_at" ? "index" : "value"));
        const auto name = kind == "list_at" ? "at" : "append";
        return emit(std::string(name) + "(" + values + "," + value + ")");
    }
    if (kind == "list_filter_by_text") {
        const auto& filter = member(node, "list_filter_by_text");
        const auto values = sub(member(filter, "values"));
        const auto key = sub(member(filter, "key"));
        const auto value = temporary(cppType, "{}");
        const auto position = positionMember(filter, "position");
        const auto& record = member(member(member(member(filter, "values"), "type"), "list"), "element");
        if (static_cast<std::size_t>(position) >= arrayMember(member(record, "record"), "fields").size())
            throw std::runtime_error("filter field position out of range");
        append("for(const auto& row:" + values + "){if(row.f" + std::to_string(position)
            + "==" + key + ")" + value + ".push_back(row);}\n");
        return value;
    }
    if (kind == "optional_some") {
        const auto value = sub(member(member(node, "some"), "value"));
        return emit(cppType + "{true," + value + "}");
    }
    if (kind == "optional_has_value") {
        const auto value = sub(member(member(node, "has_value"), "value"));
        return emit(value + ".present");
    }
    if (kind == "result_ok") {
        const auto value = sub(member(member(node, "result_ok"), "value"));
        return emit("success(" + value + ")");
    }
    if (kind == "result_err") {
        const auto value = sub(member(member(node, "result_err"), "error"));
        return emit("failure<std::int64_t>(" + value + ")");
    }
    if (kind == "result_is_ok") {
        const auto value = sub(member(member(node, "result_is_ok"), "value"));
        return emit(value + ".ok");
    }
    if (kind == "optional_value_or" || kind == "result_success_or" || kind == "result_failure_or") {
        const auto* payload = "value_or";
        std::string flag = "present";
        std::string field = "value";
        if (kind == "result_success_or") {
            payload = "result_success_or";
            flag = "ok";
        } else if (kind == "result_failure_or") {
            payload = "result_failure_or";
            flag = "!ok";
            field = "error";
        }
        const auto& operation = member(node, payload);
        const auto input = sub(member(operation, "value"));
        // Preserve the reference backend's eager fallback evaluation.
        const auto fallback = sub(member(operation, "fallback"));
        const auto condition = flag == "!ok" ? "!" + input + ".ok" : input + "." + flag;
        return emit(condition + "?" + input + "." + field + ":" + fallback);
    }
    if (kind == "propagate") {
        const auto& propagate = member(node, "propagate");
        const auto value = sub(member(propagate, "value"));
        if (textMember(member(propagate, "carrier"), "kind") == "optional")
            append("if(!" + value + ".present)return {};\n");
        else
            append("if(!" + value + ".ok)return " + returnType_ + "{false,{}," + value + ".error};\n");
        return emit(value + ".value");
    }
    if (kind == "match") {
        const auto& match = member(node, "match");
        const auto input = sub(member(match, "value"));
        const auto value = temporary(cppType, "{}");
        const auto& arms = arrayMember(match, "arms");
        for (std::size_t index = 0; index < arms.size(); ++index) {
            const auto& arm = arms[index];
            const auto tag = textMember(arm, "tag");
            std::string condition = input + ".ok";
            std::string field = "value";
            if (tag == "Err" || tag == "err") {
                condition = "!" + input + ".ok";
                field = "error";
            } else if (tag == "Some" || tag == "some") {
                condition = input + ".present";
            } else if (tag == "None" || tag == "none") {
                condition = "!" + input + ".present";
            } else if (tag != "Ok" && tag != "ok") {
                throw std::runtime_error("unsupported match tag");
            }
            if (index > 0)
                append("else ");
            append("if(" + condition + "){\n");
            auto nested = scope;
            if (arm.contains("binding") && !arm.at("binding").is_null())
                nested[positionMember(arm, "binding")] = input + "." + field;
            const auto body = expression(member(arm, "body"), nested);
            append(value + "=" + body + ";\n}\n");
        }
        return value;
    }
    throw std::runtime_error("unsupported C++ pilot operation " + kind);
}

}
