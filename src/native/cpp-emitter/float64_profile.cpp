#include "float64_profile.hpp"
#include "float64_support.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pipelang::native {
namespace {

struct Expression {
    std::string kind;
    std::string type;
    int parameter = 0;
    bool boolean = false;
    std::string bits;
    std::string operation;
    std::vector<Expression> operands;
};

struct Function {
    std::string package;
    std::string path;
    std::vector<std::string> parameters;
    Expression body;
};

void exactMembers(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size())
        throw std::runtime_error("invalid float64 profile object members");
    for (const auto name : names) {
        if (!object.contains(name))
            throw std::runtime_error("missing float64 profile member");
    }
}

std::string typeTag(const Json& value) {
    if (!value.is_string())
        throw std::runtime_error("float64 profile type must be a string");
    const auto type = value.get<std::string>();
    if (type != "bool" && type != "float64")
        throw std::runtime_error("unsupported float64 profile type");
    return type;
}

std::string identityMember(const Json& object, const char* name) {
    auto value = textMember(object, name);
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos)
        throw std::runtime_error("invalid float64 profile identity");
    return value;
}

std::string finiteBits(const Json& object) {
    const auto bits = textMember(object, "float64_bits");
    if (bits.size() != 16)
        throw std::runtime_error("binary64 bits require 16 lowercase hexadecimal digits");
    std::uint64_t value = 0;
    for (const auto digit : bits) {
        unsigned nibble;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<unsigned>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<unsigned>(digit - 'a' + 10);
        } else {
            throw std::runtime_error("invalid binary64 hexadecimal digit");
        }
        value = (value << 4) | nibble;
    }
    if ((value & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000))
        throw std::runtime_error("binary64 literal must be finite");
    return bits;
}

bool comparisonAllowed(const std::string& operation, const std::string& operandType) {
    if (operation == "and" || operation == "or")
        return operandType == "bool";
    if (operation == "equal" || operation == "not_equal")
        return operandType == "bool" || operandType == "float64";
    if (operation == "less_than" || operation == "less_or_equal"
        || operation == "greater_than" || operation == "greater_or_equal")
        return operandType == "float64";
    return false;
}

Expression decodeExpression(const Json& input, const std::vector<std::string>& parameters,
    int depth, std::size_t& nodes) {
    ++nodes;
    if (depth > 64 || nodes > 4096)
        throw std::runtime_error("float64 expression resource bound");
    Expression expression;
    expression.kind = textMember(input, "kind");
    expression.type = typeTag(member(input, "type"));
    if (expression.kind == "reference") {
        exactMembers(input, {"kind", "type", "parameter"});
        expression.parameter = positionMember(input, "parameter");
        if (static_cast<std::size_t>(expression.parameter) >= parameters.size()
            || parameters[expression.parameter] != expression.type)
            throw std::runtime_error("float64 reference type or position mismatch");
    } else if (expression.kind == "literal") {
        if (expression.type == "float64") {
            exactMembers(input, {"kind", "type", "float64_bits"});
            expression.bits = finiteBits(input);
        } else {
            exactMembers(input, {"kind", "type", "boolean"});
            const auto& value = member(input, "boolean");
            if (!value.is_boolean())
                throw std::runtime_error("expected Boolean literal");
            expression.boolean = value.get<bool>();
        }
    } else if (expression.kind == "unary" || expression.kind == "binary") {
        exactMembers(input, {"kind", "type", "operator", "operands"});
        expression.operation = textMember(input, "operator");
        const auto& operands = member(input, "operands");
        const std::size_t count = expression.kind == "unary" ? 1 : 2;
        if (expression.type != "bool" || !operands.is_array() || operands.size() != count)
            throw std::runtime_error("invalid float64 expression result or operands");
        for (const auto& operand : operands)
            expression.operands.push_back(decodeExpression(operand, parameters, depth + 1, nodes));
        const auto& operandType = expression.operands.front().type;
        if (expression.kind == "unary") {
            if (expression.operation != "not" || operandType != "bool")
                throw std::runtime_error("float64 profile supports only logical not");
        } else if (operandType != expression.operands.back().type
            || !comparisonAllowed(expression.operation, operandType)) {
            throw std::runtime_error("float64 profile operator or operand mismatch");
        }
    } else {
        throw std::runtime_error("unsupported float64 expression kind");
    }
    return expression;
}

std::string functionKey(const Function& function) {
    return function.package + '\0' + function.path;
}

std::vector<Function> decodeFunctions(const Json& request) {
    exactMembers(request, {"contract", "profile", "language_contract", "compiler_contract", "functions"});
    if (textMember(request, "contract") != float64RequestContract
        || textMember(request, "profile") != float64Profile
        || textMember(request, "language_contract") != "v0.113.0"
        || textMember(request, "compiler_contract") != "pipelang.compiler.v1")
        throw std::runtime_error("unsupported float64 emitter contract or profile");
    const auto& functions = member(request, "functions");
    if (!functions.is_array() || functions.empty() || functions.size() > 128)
        throw std::runtime_error("float64 profile requires 1..128 functions");
    std::vector<Function> result;
    std::set<std::string> identities;
    std::size_t nodes = 0;
    for (const auto& input : functions) {
        exactMembers(input, {"package_id", "path", "parameters", "body"});
        Function function;
        function.package = identityMember(input, "package_id");
        function.path = identityMember(input, "path");
        if (!identities.insert(functionKey(function)).second)
            throw std::runtime_error("duplicate float64 function identity");
        const auto& parameters = member(input, "parameters");
        if (!parameters.is_array() || parameters.size() > 64)
            throw std::runtime_error("invalid float64 function parameter count");
        for (const auto& parameter : parameters)
            function.parameters.push_back(typeTag(parameter));
        function.body = decodeExpression(member(input, "body"), function.parameters, 1, nodes);
        if (function.body.type != "bool")
            throw std::runtime_error("float64 comparison functions must return bool");
        result.push_back(std::move(function));
    }
    std::sort(result.begin(), result.end(), [](const Function& first, const Function& second) {
        return functionKey(first) < functionKey(second);
    });
    return result;
}

class Float64Generator {
public:
    Json generate(const std::vector<Function>& functions) {
        source_ = "// Generated by pipelang.cpp.float64-comparison.v1. C++17 / IEEE binary64 comparisons.\n";
        source_ += float64RuntimeSupport;
        source_ += "\nnamespace pipelang {\n";
        Json bindings = Json::array();
        for (const auto& function : functions) {
            const auto symbol = symbolName("fn_", functionKey(function));
            std::string parameters;
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0)
                    parameters += ", ";
                parameters += function.parameters[index] == "float64" ? "double" : "bool";
                parameters += " p" + std::to_string(index);
            }
            line("inline bool " + symbol + "(" + parameters + ")");
            line("{");
            ++indent_;
            const auto value = expression(function.body);
            line("return " + value + ";");
            --indent_;
            line("}");
            bindings.push_back({{"package_id", function.package}, {"path", function.path}, {"name", symbol}});
        }
        line("}");
        return {{"contract", "pipelang.cpp-emitter.response.v2"}, {"profile", float64Profile},
                {"source", source_}, {"functions", std::move(bindings)}};
    }

private:
    std::string source_;
    std::size_t indent_ = 0;
    std::size_t temporaries_ = 0;

    void line(const std::string& text) {
        const auto addition = std::string(indent_ * 4, ' ') + text + '\n';
        if (addition.size() > byteLimit - source_.size())
            throw std::runtime_error("float64 generated source byte limit");
        source_ += addition;
    }

    std::string temporary(const std::string& type, const std::string& value) {
        const auto name = "v" + std::to_string(++temporaries_);
        line(type + " " + name + " = " + value + ";");
        return name;
    }

    std::string expression(const Expression& node) {
        if (node.kind == "reference")
            return "p" + std::to_string(node.parameter);
        if (node.kind == "literal") {
            if (node.type == "float64")
                return temporary("const double", "float64_comparison_detail::from_bits(UINT64_C(0x" + node.bits + "))");
            return temporary("const bool", node.boolean ? "true" : "false");
        }
        if (node.kind == "unary")
            return temporary("const bool", "!" + expression(node.operands.front()));
        const auto left = expression(node.operands.front());
        if (node.operation == "and" || node.operation == "or") {
            const auto result = temporary("bool", left);
            const auto condition = node.operation == "or" ? "!" + result : result;
            line("if (" + condition + ") {");
            ++indent_;
            const auto right = expression(node.operands.back());
            line(result + " = " + right + ";");
            --indent_;
            line("}");
            return result;
        }
        const auto right = expression(node.operands.back());
        static const std::map<std::string, std::string> operators = {
            {"equal", "=="}, {"not_equal", "!="}, {"less_than", "<"},
            {"less_or_equal", "<="}, {"greater_than", ">"}, {"greater_or_equal", ">="}
        };
        return temporary("const bool", left + " " + operators.at(node.operation) + " " + right);
    }
};

}

Json generateFloat64Comparison(const Json& request) {
    return Float64Generator{}.generate(decodeFunctions(request));
}

}
