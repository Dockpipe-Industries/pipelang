// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

namespace pipelang::legacy {
namespace {
bool numeric(const std::string& type) {
    return type == "int" || type == "float";
}
[[noreturn]] void invalid(const Expr& expression, const std::string& message, bool evaluation = false) {
    throw Diagnostic(evaluation ? "PL4002" : "PL3009", message, expression->file, expression->start, expression->end);
}
std::string operationType(const Expr& expression, const std::vector<std::string>& arguments, bool evaluation = false) {
    const auto& op = expression->text;
    const auto& first = arguments.front();
    if (expression->kind == "unary") {
        if (op == "!" && first == "bool")
            return "bool";
        if (op == "-" && numeric(first))
            return first;
        if (op == "!")
            invalid(expression, "operator ! expects bool" + (evaluation ? std::string{} : ", got " + first), evaluation);
        if (op == "-")
            invalid(expression, "operator - expects int or float" + (evaluation ? std::string{} : ", got " + first), evaluation);
        invalid(expression, "unsupported unary operator " + Json(op).dump(), evaluation);
    }
    const auto& second = arguments.back();
    if (op == "+" && first == "string" && second == "string")
        return "string";
    if ((op == "+" || op == "-" || op == "*" || op == "/") && numeric(first) && numeric(second)) {
        if (op == "/" || first == "float" || second == "float")
            return "float";
        return "int";
    }
    if ((op == "<" || op == "<=" || op == ">" || op == ">=") && numeric(first) && numeric(second))
        return "bool";
    if ((op == "==" || op == "!=") && first == second)
        return "bool";
    if ((op == "&&" || op == "||") && first == "bool" && second == "bool")
        return "bool";
    if (!evaluation && (op == "<" || op == "<=" || op == ">" || op == ">=") && first == second && !numeric(first) && first != "bool" && first != "string")
        invalid(expression, "IComparable is not implemented yet for type " + first);
    invalid(expression, evaluation ? "invalid operands for " + Json(op).dump() : "invalid operand types for " + Json(op).dump() + ": " + first + " and " + second, evaluation);
}
std::string inferAt(const Expr& expression, const std::map<std::string, std::string>& types, unsigned depth) {
    require(expression && depth <= 128, "legacy expression depth limit exceeded");
    if (expression->kind == "literal")
        return expression->literal.type;
    if (expression->kind == "identifier") {
        const auto found = types.find(expression->text);
        if (found == types.end())
            invalid(expression, "unknown identifier " + Json(expression->text).dump());
        return found->second;
    }
    std::vector<std::string> arguments;
    for (const auto& child : expression->children)
        arguments.push_back(inferAt(child, types, depth + 1));
    return operationType(expression, arguments);
}
Value evaluateAt(const Expr& expression, const std::map<std::string, Value>& values, unsigned depth) {
    require(expression && depth <= 128, "legacy evaluation depth limit exceeded");
    if (expression->kind == "literal")
        return expression->literal;
    if (expression->kind == "identifier") {
        const auto found = values.find(expression->text);
        if (found == values.end())
            invalid(expression, "unknown identifier " + Json(expression->text).dump(), true);
        return found->second;
    }
    std::vector<Value> arguments;
    std::vector<std::string> types;
    for (const auto& child : expression->children) {
        arguments.push_back(evaluateAt(child, values, depth + 1));
        types.push_back(arguments.back().type);
    }
    const auto resultType = operationType(expression, types, true);
    const auto& op = expression->text;
    const auto& first = arguments.front();
    if (expression->kind == "unary") {
        if (op == "!")
            return {"bool", !first.data.get<bool>()};
        if (resultType == "float")
            return {"float", -first.data.get<double>()};
        const auto bits = std::bit_cast<std::uint64_t>(first.data.get<std::int64_t>());
        return {"int", std::bit_cast<std::int64_t>(std::uint64_t{0} - bits)};
    }
    const auto& second = arguments.back();
    if (resultType == "string")
        return {"string", first.data.get<std::string>() + second.data.get<std::string>()};
    if (op == "==" || op == "!=") {
        const bool equal = !first.type.empty() && first.data == second.data;
        return {"bool", op == "==" ? equal : !equal};
    }
    if (op == "&&" || op == "||") {
        const bool left = first.data.get<bool>();
        const bool right = second.data.get<bool>();
        return {"bool", op == "&&" ? left && right : left || right};
    }
    if (resultType == "int") {
        const auto left = std::bit_cast<std::uint64_t>(first.data.get<std::int64_t>());
        const auto right = std::bit_cast<std::uint64_t>(second.data.get<std::int64_t>());
        std::uint64_t result = 0;
        if (op == "+")
            result = left + right;
        else if (op == "-")
            result = left - right;
        else
            result = left * right;
        return {"int", std::bit_cast<std::int64_t>(result)};
    }
    const double left = first.data.get<double>();
    const double right = second.data.get<double>();
    if (op == "+")
        return {"float", left + right};
    if (op == "-")
        return {"float", left - right};
    if (op == "*")
        return {"float", left * right};
    if (op == "/") {
        if (right == 0)
            invalid(expression, "division by zero", true);
        return {"float", left / right};
    }
    if (op == "<")
        return {"bool", left < right};
    if (op == "<=")
        return {"bool", left <= right};
    if (op == ">")
        return {"bool", left > right};
    return {"bool", left >= right};
}
}
std::string Value::string() const {
    if (type.empty())
        return {};
    if (type == "string")
        return data.get<std::string>();
    if (type == "float") {
        const auto number = data.get<double>();
        if (std::isnan(number))
            return "NaN";
        if (std::isinf(number))
            return number < 0 ? "-Inf" : "+Inf";
        std::array<char, 128> output{};
        const auto converted = std::to_chars(output.data(), output.data() + output.size(), number, std::chars_format::general);
        require(converted.ec == std::errc{}, "cannot format legacy float");
        return {output.data(), converted.ptr};
    }
    return data.dump();
}
Value evaluate(const Expr& expression, const std::map<std::string, Value>& values) {
    return evaluateAt(expression, values, 0);
}
std::string infer(const Expr& expression, const std::map<std::string, std::string>& types) {
    return inferAt(expression, types, 0);
}
Value argument(const std::string& type, const std::string& value) {
    if (type == "string")
        return {type, value};
    if (type == "bool") {
        if (value == "true" || value == "True" || value == "TRUE" || value == "t" || value == "T" || value == "1")
            return {type, true};
        require(value == "false" || value == "False" || value == "FALSE" || value == "f" || value == "F" || value == "0", "expected bool");
        return {type, false};
    }
    if (type == "int") {
        std::int64_t number = 0;
        auto begin = value.data();
        if (!value.empty() && value.front() == '+')
            ++begin;
        const auto result = std::from_chars(begin, value.data() + value.size(), number);
        require(result.ec == std::errc{} && result.ptr == value.data() + value.size(), "expected int");
        return {type, number};
    }
    require(type == "float", "unsupported legacy argument type");
    if (value == "NaN")
        return {type, std::numeric_limits<double>::quiet_NaN()};
    if (value == "+Inf" || value == "Inf" || value == "+Infinity" || value == "Infinity")
        return {type, std::numeric_limits<double>::infinity()};
    if (value == "-Inf" || value == "-Infinity")
        return {type, -std::numeric_limits<double>::infinity()};
    double number = 0;
    auto begin = value.data();
    if (!value.empty() && value.front() == '+')
        ++begin;
    const auto result = std::from_chars(begin, value.data() + value.size(), number);
    require(result.ec == std::errc{} && result.ptr == value.data() + value.size(), "expected float");
    return {type, number};
}
std::string expressionText(const Expr& expression) {
    if (!expression)
        return {};
    if (expression->kind == "literal")
        return expression->literal.type == "string" ? expression->literal.data.dump() : expression->literal.string();
    if (expression->kind == "identifier")
        return expression->text;
    if (expression->kind == "unary")
        return expression->text + expressionText(expression->children[0]);
    return "(" + expressionText(expression->children[0]) + " " + expression->text + " " + expressionText(expression->children[1]) + ")";
}
}
