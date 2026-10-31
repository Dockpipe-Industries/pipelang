// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "scalar.hpp"
#include "../model-compiler/syntax.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>

namespace pipelang::scalar {
namespace {
using namespace tools;
std::string segment(const std::string& name) {
    require(std::regex_match(name, std::regex("[A-Za-z][A-Za-z0-9]*")), "scalar semantic names require letters and digits");
    auto result = name;
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    return result;
}
std::string tag(const models::Type& type) {
    require(type.arguments.empty() && (type.name == "bool" || type.name == "float"), "scalar functions require exact bool or binary64 types");
    return type.name == "bool" ? "bool" : "float64";
}
Json coreType(const std::string& type) {
    if (type == "bool")
        return {{"kind", "primitive"}, {"primitive", "bool"}};
    return {{"kind", "numeric"}, {"numeric", {{"representation", "binary_float"}, {"bits", 64}}}};
}
Json sourceType(const std::string& type) {
    return {{"kind", "primitive"}, {"primitive", type == "bool" ? "bool" : "float"}};
}
struct Function {
    const models::Method* method = nullptr;
    std::string owner;
    std::string path;
    std::string result;
    std::vector<std::string> parameters;
};
class Projection {
public:
    Projection(models::Program program, std::string package, const std::string& module)
        : program_(std::move(program)), package_(std::move(package)) {
        const std::regex dotted("[a-z][a-z0-9]*(\\.[a-z][a-z0-9]*)*");
        require(std::regex_match(package_, dotted) && std::regex_match(module, dotted), "scalar package and module require stable lowercase dotted identities");
        require(program_.module.empty() && program_.imports.empty(), "scalar source is one explicit offline compilation unit");
        std::set<std::string> owners;
        std::set<std::string> identities;
        for (const auto& owner : program_.declarations) {
            require(owner.kind == "Class" && owner.fields.empty() && owner.interfaces.empty() && owner.typeParameters.empty(), "scalar source requires stateless nongeneric classes");
            require(owners.insert(owner.name).second, "duplicate scalar class");
            for (const auto& method : owner.methods) {
                require(!method.constructor && !method.asynchronous && method.typeParameters.empty(), "scalar functions exclude constructors, tasks and generics");
                require(method.body.size() == 1 && method.body[0].kind == "return" && method.body[0].expression, "scalar functions require one expression return");
                Function function;
                function.method = &method;
                function.owner = owner.name;
                function.path = module + '.' + segment(owner.name) + '.' + segment(method.name);
                function.result = tag(method.result);
                require(identities.insert(function.path).second, "duplicate scalar semantic identity");
                std::set<std::string> parameters;
                for (const auto& parameter : method.parameters) {
                    require(parameters.insert(parameter.name).second, "duplicate scalar parameter");
                    function.parameters.push_back(tag(parameter.type));
                }
                require(function.parameters.size() <= 64 && functions_.size() < 128, "scalar function/parameter capacity exceeded");
                const auto key = owner.name + '\0' + method.name;
                require(functions_.emplace(key, std::move(function)).second, "duplicate scalar method");
            }
        }
        require(!functions_.empty(), "scalar source requires callable functions");
    }
    Compilation run() {
        std::map<std::string, Json> ordered;
        for (const auto& [name, function] : functions_) {
            std::map<std::string, std::pair<std::size_t, std::string>> parameters;
            for (std::size_t index = 0; index < function.parameters.size(); ++index)
                parameters.emplace(function.method->parameters[index].name, std::make_pair(index, function.parameters[index]));
            auto body = expression(function.method->body[0].expression, function, parameters, 1);
            require(body.at("type") == function.result, "scalar return type differs from its declaration");
            ordered.emplace(function.path, Json{{"package_id", package_}, {"path", function.path}, {"return_type", function.result},
                {"parameters", function.parameters}, {"body", std::move(body)}});
        }
        Compilation result;
        result.request = {{"contract", "pipelang.cpp-emitter.request.v4"}, {"profile", profile}, {"language_contract", "v0.113.0"},
            {"compiler_contract", "pipelang.compiler.v1"}, {"functions", Json::array()}};
        result.functions = Json::array();
        for (const auto& [path, function] : ordered)
            result.request["functions"].push_back(function);
        for (const auto& [path, projected] : ordered) {
            const auto found = std::find_if(functions_.begin(), functions_.end(), [&](const auto& item) { return item.second.path == path; });
            require(found != functions_.end(), "scalar metadata lost its source declaration");
            const auto& function = found->second;
            if (!function.method->visible)
                continue;
            Json signature = Json::array();
            Json parameters = Json::array();
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                signature.push_back(sourceType(function.parameters[index]));
                parameters.push_back({{"position", index}, {"name", function.method->parameters[index].name}, {"type", coreType(function.parameters[index])}});
            }
            const auto identity = Json{{"package_id", package_}, {"path", path},
                {"callable", {{"parameters", signature}, {"returns", sourceType(function.result)}}}};
            result.functions.push_back({{"identity", identity}, {"name", function.method->name},
                {"cpp_symbol", "pipelang::fn_" + sha256(package_ + '\0' + path).substr(0, 24)},
                {"return_type", coreType(function.result)}, {"parameters", parameters}});
        }
        require(!result.functions.empty(), "scalar source requires a public entrypoint");
        return result;
    }
private:
    Json expression(const models::Expr& node, const Function& owner,
                    const std::map<std::string, std::pair<std::size_t, std::string>>& parameters, unsigned depth) {
        require(node && depth <= 64 && ++nodes_ <= 4096, "scalar expression depth/node capacity exceeded");
        const auto child = [&](std::size_t index) { return expression(node->children.at(index), owner, parameters, depth + 1); };
        if (node->kind == "name") {
            const auto found = parameters.find(node->text);
            require(found != parameters.end(), "scalar expression captures an unknown or stateful value");
            return {{"kind", "reference"}, {"type", found->second.second}, {"parameter", found->second.first}};
        }
        if (node->kind == "boolean")
            return {{"kind", "literal"}, {"type", "bool"}, {"boolean", node->text == "true"}};
        if (node->kind == "float" || (node->kind == "unary" && node->text == "-" && node->children.at(0)->kind == "float")) {
            const auto& literal = node->kind == "float" ? node->text : node->children[0]->text;
            double value = 0;
            const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), value);
            require(parsed.ec == std::errc{} && parsed.ptr == literal.data() + literal.size() && std::isfinite(value), "scalar literal must be finite binary64");
            auto bits = std::bit_cast<std::uint64_t>(value);
            if (node->kind == "unary")
                bits ^= UINT64_C(0x8000000000000000);
            std::ostringstream encoded;
            encoded << std::hex << std::setfill('0') << std::setw(16) << bits;
            return {{"kind", "literal"}, {"type", "float64"}, {"float64_bits", encoded.str()}};
        }
        if (node->kind == "unary") {
            const auto operand = child(0);
            require(node->text == "!" && operand.at("type") == "bool", "scalar profile admits only logical negation and finite float literals");
            return {{"kind", "unary"}, {"type", "bool"}, {"operator", "not"}, {"operands", Json::array({operand})}};
        }
        if (node->kind == "binary") {
            static const std::map<std::string, std::string> operations{{"<", "less_than"}, {"<=", "less_or_equal"}, {">", "greater_than"},
                {">=", "greater_or_equal"}, {"==", "equal"}, {"!=", "not_equal"}, {"&&", "and"}, {"||", "or"}};
            const auto operation = operations.find(node->text);
            require(operation != operations.end(), "scalar profile excludes arithmetic operators");
            const auto left = child(0);
            const auto right = child(1);
            require(left.at("type") == right.at("type"), "scalar binary operands have different types");
            const bool logical = node->text == "&&" || node->text == "||";
            const bool ordered = node->text == "<" || node->text == "<=" || node->text == ">" || node->text == ">=";
            require((!logical || left.at("type") == "bool") && (!ordered || left.at("type") == "float64"), "scalar operator has invalid operand types");
            return {{"kind", "binary"}, {"type", "bool"}, {"operator", operation->second}, {"operands", Json::array({left, right})}};
        }
        if (node->kind == "conditional") {
            const auto condition = child(0);
            const auto first = child(1);
            const auto second = child(2);
            require(condition.at("type") == "bool" && first.at("type") == second.at("type"), "scalar conditional has inconsistent types");
            return {{"kind", "conditional"}, {"type", first.at("type")}, {"operands", Json::array({condition, first, second})}};
        }
        if (node->kind == "call") {
            require(!node->children.empty() && node->children[0]->kind == "name", "scalar calls require a same-class named helper");
            const auto found = functions_.find(owner.owner + '\0' + node->children[0]->text);
            require(found != functions_.end(), "scalar helper is not declared");
            const auto& target = found->second;
            require(node->children.size() - 1 == target.parameters.size(), "scalar helper argument count differs");
            Json arguments = Json::array();
            for (std::size_t index = 1; index < node->children.size(); ++index) {
                const auto argument = child(index);
                require(argument.at("type") == target.parameters[index - 1], "scalar helper argument type differs");
                arguments.push_back(argument);
            }
            return {{"kind", "call"}, {"type", target.result}, {"callee", {{"package_id", package_}, {"path", target.path}}}, {"arguments", arguments}};
        }
        throw std::runtime_error("unsupported operation in the stateless scalar profile: " + node->kind);
    }
    models::Program program_;
    std::string package_;
    std::map<std::string, Function> functions_;
    std::size_t nodes_ = 0;
};
}
Compilation compile(const std::string& source, const std::string& package, const std::string& module) {
    tools::require(!source.empty() && source.size() <= 65536, "scalar source must contain 1..65536 bytes");
    return Projection(models::parse(source, false, 64), package, module).run();
}
}
