// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <pipelang/model/binary32.hpp>
#include <pipelang/model/uint32.hpp>

#include <bit>
#include <iomanip>
#include <sstream>
#include <variant>

namespace pipelang::models {
namespace {

using Value = std::variant<bool, std::int64_t, std::uint32_t, float, double>;

[[noreturn]] void refuse(const Expr& expression, const std::string& message) {
    throw Diagnostic(message, expression->offset);
}

Value evaluate(const Expr& expression, unsigned depth = 0);

template<class Number>
Value binary(const Expr& expression, Number left, Number right) {
    const auto& operation = expression->text;
    if (operation == "==")
        return left == right;
    if (operation == "!=")
        return left != right;
    if (operation == "<")
        return left < right;
    if (operation == "<=")
        return left <= right;
    if (operation == ">")
        return left > right;
    if (operation == ">=")
        return left >= right;
    if constexpr (std::is_same_v<Number, bool>) {
        refuse(expression, "unsupported boolean constant operation");
    } else if constexpr (std::is_integral_v<Number>) {
        if (operation == "+")
            return pipelang::model::add(left, right);
        if (operation == "-")
            return pipelang::model::subtract(left, right);
        if (operation == "*")
            return pipelang::model::multiply(left, right);
        if (operation == "/")
            return pipelang::model::divide(left, right);
        if (operation == "%")
            return pipelang::model::remainder(left, right);
        if constexpr (std::is_same_v<Number, std::uint32_t>) {
            if (operation == "&")
                return pipelang::model::bitAnd(left, right);
            if (operation == "|")
                return pipelang::model::bitOr(left, right);
            if (operation == "^")
                return pipelang::model::bitXor(left, right);
        }
    } else {
        // Materializing each result preserves the same nominal-width boundary
        // as generated runtime arithmetic, including IEEE infinities and NaNs.
        if (operation == "+") {
            volatile Number result = left + right;
            return Number(result);
        }
        if (operation == "-") {
            volatile Number result = left - right;
            return Number(result);
        }
        if (operation == "*") {
            volatile Number result = left * right;
            return Number(result);
        }
        if (operation == "/") {
            volatile Number result = left / right;
            return Number(result);
        }
    }
    refuse(expression, "unsupported numeric constant operation");
}

Value conversion(const Expr& expression, const Value& argument) {
    return std::visit([&](auto value) -> Value {
        using Input = decltype(value);
        const auto& target = expression->type.name;
        if constexpr (!std::is_same_v<Input, bool>) {
            if (target == "int") {
                if constexpr (std::is_same_v<Input, std::uint32_t>)
                    return pipelang::model::widenUInt32(value);
                if constexpr (std::is_floating_point_v<Input>)
                    return pipelang::model::truncateToInt(value);
            }
            if (target == "uint32") {
                if constexpr (std::is_same_v<Input, std::int64_t>)
                    return pipelang::model::checkedUInt32(value);
                if constexpr (std::is_floating_point_v<Input>)
                    return pipelang::model::truncateToUInt32(value);
            }
            if (target == "float32") {
                if constexpr (std::is_same_v<Input, std::int64_t>)
                    return pipelang::model::roundInteger<float>(value);
                if constexpr (std::is_same_v<Input, std::uint32_t>)
                    return pipelang::model::roundUInt32<float>(value);
                if constexpr (std::is_same_v<Input, double>)
                    return pipelang::model::narrowFloat(value);
            }
            if (target == "float") {
                if constexpr (std::is_same_v<Input, std::int64_t>)
                    return pipelang::model::roundInteger<double>(value);
                if constexpr (std::is_same_v<Input, std::uint32_t>)
                    return pipelang::model::roundUInt32<double>(value);
                if constexpr (std::is_same_v<Input, float>)
                    return pipelang::model::widenFloat(value);
            }
        }
        refuse(expression, "unsupported constant conversion");
    }, argument);
}

Value evaluate(const Expr& expression, unsigned depth) {
    if (depth > 64)
        refuse(expression, "constant expression depth exceeds 64");
    if (expression->resolution == "constant")
        return evaluate(expression->constantValue, depth + 1);
    if (expression->resolution == "minimum_int")
        return std::numeric_limits<std::int64_t>::min();
    if (expression->kind == "boolean")
        return expression->text == "true";
    if (expression->kind == "integer")
        return std::int64_t(std::stoll(expression->text));
    if (expression->kind == "uint32")
        return std::uint32_t(std::stoull(expression->resolution));
    if (expression->resolution == "ieee_constant") {
        if (expression->type.name == "float32") {
            if (expression->text == "PositiveInfinity")
                return std::bit_cast<float>(UINT32_C(0x7f800000));
            if (expression->text == "NegativeInfinity")
                return std::bit_cast<float>(UINT32_C(0xff800000));
            return std::bit_cast<float>(UINT32_C(0x7fc00000));
        }
        if (expression->text == "PositiveInfinity")
            return std::bit_cast<double>(UINT64_C(0x7ff0000000000000));
        if (expression->text == "NegativeInfinity")
            return std::bit_cast<double>(UINT64_C(0xfff0000000000000));
        return std::bit_cast<double>(UINT64_C(0x7ff8000000000000));
    }
    if (expression->kind == "float32")
        return std::bit_cast<float>(std::uint32_t(std::stoull(expression->resolution)));
    if (expression->kind == "float")
        return std::bit_cast<double>(std::stoull(expression->resolution));
    if (expression->kind == "conditional") {
        const bool condition = std::get<bool>(evaluate(expression->children[0], depth + 1));
        return evaluate(expression->children[condition ? 1 : 2], depth + 1);
    }
    if (expression->kind == "unary") {
        const auto operand = evaluate(expression->children[0], depth + 1);
        return std::visit([&](auto value) -> Value {
            using Number = decltype(value);
            if constexpr (std::is_same_v<Number, bool>) {
                return !value;
            } else if constexpr (std::is_same_v<Number, std::uint32_t>) {
                return pipelang::model::bitNot(value);
            } else if constexpr (std::is_same_v<Number, std::int64_t>) {
                return pipelang::model::negate(value);
            } else {
                volatile Number result = -value;
                return Number(result);
            }
        }, operand);
    }
    if (expression->kind == "binary") {
        const auto left = evaluate(expression->children[0], depth + 1);
        if (expression->text == "&&" && !std::get<bool>(left))
            return false;
        if (expression->text == "||" && std::get<bool>(left))
            return true;
        const auto right = evaluate(expression->children[1], depth + 1);
        if (expression->text == "&&" || expression->text == "||")
            return std::get<bool>(right);
        if (expression->text == "<<")
            return pipelang::model::shiftLeft(std::get<std::uint32_t>(left), std::get<std::int64_t>(right));
        if (expression->text == ">>")
            return pipelang::model::shiftRight(std::get<std::uint32_t>(left), std::get<std::int64_t>(right));
        return std::visit([&](auto value) -> Value {
            return binary(expression, value, std::get<decltype(value)>(right));
        }, left);
    }
    if (expression->resolution == "numeric_conversion" || expression->resolution == "unsigned_conversion")
        return conversion(expression, evaluate(expression->children[1], depth + 1));
    refuse(expression, "constant initializer requires closed scalar values and explicit numeric conversions");
}

std::string ownerIdentity(const Declaration& declaration) {
    auto name = declaration.sourceName.empty() ? declaration.name : declaration.sourceName;
    const auto separator = name.rfind('.');
    if (separator != std::string::npos)
        name = name.substr(separator + 1);
    std::string identity = declaration.module;
    std::size_t position = 0;
    while ((position = identity.find('.', position)) != std::string::npos) {
        identity.replace(position, 1, "::");
        position += 2;
    }
    if (!identity.empty())
        identity += "::";
    return identity + name;
}

std::string scalarCppType(const Type& type) {
    if (type.name == "int")
        return "std::int64_t";
    if (type.name == "uint32")
        return "std::uint32_t";
    if (type.name == "float")
        return "double";
    if (type.name == "float32")
        return "float";
    return "bool";
}

}

Expr foldConstant(const Expr& expression) {
    try {
        pipelang::model::requireBinary32Environment();
        const auto value = evaluate(expression);
        auto literal = std::make_shared<Expression>();
        literal->type = expression->type;
        literal->offset = expression->offset;
        std::visit([&](auto number) {
            using Number = decltype(number);
            if constexpr (std::is_same_v<Number, bool>) {
                literal->kind = "boolean";
                literal->text = number ? "true" : "false";
            } else if constexpr (std::is_same_v<Number, std::int64_t>) {
                literal->kind = "integer";
                literal->text = std::to_string(number);
                if (number == std::numeric_limits<std::int64_t>::min()) {
                    literal->kind = "unary";
                    literal->resolution = "minimum_int";
                }
            } else if constexpr (std::is_same_v<Number, std::uint32_t>) {
                literal->kind = "uint32";
                literal->resolution = std::to_string(number);
            } else if constexpr (std::is_same_v<Number, float>) {
                literal->kind = "float32";
                literal->resolution = std::to_string(std::bit_cast<std::uint32_t>(number));
            } else {
                literal->kind = "float";
                literal->resolution = std::to_string(std::bit_cast<std::uint64_t>(number));
            }
        }, value);
        return literal;
    } catch (const pipelang::model::ExecutionFailure& error) {
        throw Diagnostic("invalid constant: " + std::string(error.what()), expression->offset);
    }
}

std::string constantLiteral(const Expr& expression) {
    if (expression->resolution == "minimum_int")
        return "std::numeric_limits<std::int64_t>::min()";
    if (expression->kind == "boolean")
        return expression->text;
    if (expression->kind == "integer")
        return "std::int64_t{" + expression->text + "}";
    if (expression->kind == "uint32")
        return "std::uint32_t{" + expression->resolution + "U}";
    if (expression->kind == "float32")
        return "std::bit_cast<float>(std::uint32_t{" + expression->resolution + "U})";
    return "std::bit_cast<double>(std::uint64_t{" + expression->resolution + "ULL})";
}

nlohmann::json constantsManifest(const Program& program, const std::string& nameSpace) {
    auto exports = nlohmann::json::array();
    std::set<std::string> namespaceNames;
    std::map<std::string, std::pair<std::size_t, std::string>> locations;
    for (const auto& declaration : program.declarations) {
        if (declaration.generatedObjectFacet)
            continue;
        for (const auto& constant : declaration.constants) {
            if (!constant.visible)
                continue;
            const auto& literal = constant.constantValue;
            const auto cppName = nameSpace + "::constants::" + ownerIdentity(declaration) + "::" + constant.name;
            locations.emplace(cppName, std::pair{constant.offset, declaration.module});
            std::size_t separator = 0;
            while ((separator = cppName.find("::", separator)) != std::string::npos) {
                namespaceNames.insert(cppName.substr(0, separator));
                separator += 2;
            }
            static const std::map<std::string, std::string> encodings{
                {"bool", "boolean"}, {"int", "signed64-decimal"}, {"uint32", "unsigned32-decimal"},
                {"float32", "ieee754-binary32-bits"}, {"float", "ieee754-binary64-bits"}
            };
            exports.push_back({{"module", declaration.module},
                {"owner", declaration.sourceName.empty() ? declaration.name : declaration.sourceName},
                {"name", constant.name}, {"type", constant.type.text()}, {"encoding", encodings.at(constant.type.name)}, {"cpp_name", cppName},
                {"value", literal->kind == "boolean" || literal->kind == "integer" || literal->resolution == "minimum_int"
                    ? literal->text : literal->resolution}});
        }
    }
    for (const auto& [identity, location] : locations) {
        if (namespaceNames.contains(identity)) {
            Diagnostic diagnostic("constant export name conflicts with a source module or class namespace", location.first);
            diagnostic.module = location.second;
            throw diagnostic;
        }
    }
    return exports;
}

std::string constantsHeader(const Program& program, const std::string& nameSpace) {
    std::ostringstream output;
    output << "// Generated by pipelang-modelc; standalone typed constants.\n"
           << "#pragma once\n#include <bit>\n#include <cstdint>\n#include <limits>\n\n";
    const auto exports = constantsManifest(program, nameSpace);
    for (const auto& constant : exports) {
        // Identity-and-value guards allow the leaf and self-contained Model.hpp
        // to coexist, and allow distinct bindings to share a native namespace.
        // Encode the complete canonical identity without a collision-prone hash.
        std::ostringstream guard;
        guard << "PIPELANG_CONSTANT_";
        for (const unsigned char byte : constant.at("cpp_name").get<std::string>() + "/" +
                constant.at("type").get<std::string>() + "/" + constant.at("value").get<std::string>())
            guard << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
        const auto cppName = constant.at("cpp_name").get<std::string>();
        const auto separator = cppName.rfind("::");
        const auto& type = constant.at("type").get_ref<const std::string&>();
        const auto value = constant.at("value").get<std::string>();
        std::string literal;
        if (type == "bool") {
            literal = value;
        } else if (type == "int") {
            if (value == std::to_string(std::numeric_limits<std::int64_t>::min()))
                literal = "std::numeric_limits<std::int64_t>::min()";
            else
                literal = "std::int64_t{" + value + "}";
        } else if (type == "uint32") {
            literal = "std::uint32_t{" + value + "U}";
        } else if (type == "float32") {
            literal = "std::bit_cast<float>(std::uint32_t{" + value + "U})";
        } else {
            literal = "std::bit_cast<double>(std::uint64_t{" + value + "ULL})";
        }
        output << "#ifndef " << guard.str() << "\n#define " << guard.str() << "\n"
               << "namespace " << cppName.substr(0, separator) << " {\n"
               << "inline constexpr " << scalarCppType({type, {}}) << ' ' << constant.at("name").get<std::string>()
               << " = " << literal << ";\n}\n#endif\n\n";
    }
    return output.str();
}

}
