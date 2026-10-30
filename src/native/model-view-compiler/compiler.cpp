// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"
#include "../lexer/lexer.hpp"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace pipelang::model_view {
namespace {
using Token = pipelang::native::Token;
struct Failure {
    std::string message;
    std::size_t start;
    std::size_t end;
};
bool safeName(const std::string &name)
{
    static const std::set<std::string> reserved = {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else",
        "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "reflexpr", "register", "reinterpret_cast", "requires", "return", "short", "signed",
        "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "synchronized",
        "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
        "while", "xor", "xor_eq", "State", "snapshot", "poll", "pending"};
    if (name.empty() || name.size() > 64 || name.front() == '_' || reserved.count(name)
        || name.find("__") != std::string::npos || name.rfind("pl_", 0) == 0)
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        if (!letter && character != '_' && (index == 0 || character < '0' || character > '9'))
            return false;
    }
    return true;
}
bool safeNamespace(const std::string &name)
{
    std::size_t start = 0;
    while (true) {
        const auto end = name.find("::", start);
        if (!safeName(name.substr(start, end == std::string::npos ? end : end - start)))
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 2;
    }
}
bool scalar(const std::string &type)
{
    return type == "bool" || type == "int" || type == "string";
}
void members(const Json &object, const std::set<std::string> &required, const std::set<std::string> &optional = {})
{
    if (!object.is_object())
        throw std::runtime_error("expected a JSON object");
    for (const auto &name : required) {
        if (!object.contains(name))
            throw std::runtime_error("missing JSON member: " + name);
    }
    for (const auto &[name, value] : object.items()) {
        (void)value;
        if (!required.count(name) && !optional.count(name))
            throw std::runtime_error("unknown JSON member: " + name);
    }
}
std::string text(const Json &object, const char *key)
{
    const auto &value = object.at(key);
    if (!value.is_string() || value.get_ref<const std::string &>().size() > 65536)
        throw std::runtime_error(std::string("invalid string member: ") + key);
    return value.get<std::string>();
}
void boundedArray(const Json &array)
{
    if (!array.is_array() || array.size() > 128)
        throw std::runtime_error("manifest array exceeds capacity or has wrong type");
}
class Parser {
public:
    Parser(std::vector<Token> tokens, const Json &manifest) : tokens_(std::move(tokens)), manifest_(manifest) {}
    View parse(std::string nameSpace, std::string modelHeader)
    {
        const auto styleDeclarations = styles::parse(tokens_, index_);
        word("view");
        View view;
        view.name = identifier();
        view.nameSpace = std::move(nameSpace);
        view.modelHeader = std::move(modelHeader);
        take("lparen");
        word("model");
        take("colon");
        view.model = identifier();
        for (const auto *category : {"records", "enums", "interfaces", "classes"}) {
            for (const auto &item : manifest_.at(category)) {
                if (item.at("name") == view.name + "Adapter")
                    fail("view adapter name collides with a model declaration");
            }
        }
        take("rparen");
        view.style = styles::selection(styleDeclarations, tokens_, index_);
        for (const auto &item : manifest_.at("classes")) {
            if (item.at("name") == view.model)
                model_ = &item;
        }
        if (!model_)
            fail("view model must resolve a public model class");
        take("lbrace");
        view.root = element(1);
        take("rbrace");
        take("eof");
        return view;
    }
private:
    std::vector<Token> tokens_;
    const Json &manifest_;
    const Json *model_ = nullptr;
    std::size_t index_ = 0;
    std::size_t nodes_ = 0;
    std::set<std::string> identities_;
    const Token &peek() const { return tokens_.at(index_); }
    [[noreturn]] void fail(const std::string &message) const
    {
        throw Failure{message, peek().start, peek().end};
    }
    Token take(const std::string &kind)
    {
        if (peek().kind != kind)
            fail("expected " + kind);
        auto result = peek();
        if (kind != "eof")
            ++index_;
        return result;
    }
    void word(const std::string &literal)
    {
        if (take("identifier").literal != literal)
            fail("expected " + literal);
    }
    std::string identifier()
    {
        const auto token = take("identifier");
        if (!safeName(token.literal))
            throw Failure{"unsafe code identifier", token.start, token.end};
        return token.literal;
    }
    const Json *method(const std::string &name) const
    {
        for (const auto &item : model_->at("methods")) {
            if (item.at("name") == name)
                return &item;
        }
        return nullptr;
    }
    const Json *record(const std::string &name) const
    {
        for (const auto &item : manifest_.at("records")) {
            if (item.at("name") == name)
                return &item;
        }
        return nullptr;
    }
    Binding read()
    {
        word("bind");
        word("model");
        take("dot");
        std::string name = identifier();
        Binding binding;
        if (peek().kind == "lparen") {
            take("lparen");
            take("rparen");
            const auto *getter = method(name);
            if (!getter || !getter->at("pure").get<bool>() || getter->at("async").get<bool>()
                || !getter->at("parameters").empty() || !scalar(getter->at("returnType").get<std::string>()))
                fail("binding requires a public pure zero-argument scalar getter");
            binding.expression = "model_." + name + "()";
            binding.type = getter->at("returnType");
        } else {
            const Json *owner = model_;
            binding.expression = "snapshot";
            std::size_t depth = 0;
            while (true) {
                if (++depth > 8)
                    fail("field path depth limit exceeded");
                const Json *field = nullptr;
                for (const auto &item : owner->at("fields")) {
                    if (item.at("name") == name)
                        field = &item;
                }
                if (!field)
                    fail("unknown public typed model field: " + name);
                binding.expression += "." + name;
                binding.type = field->at("type");
                if (peek().kind != "dot")
                    break;
                take("dot");
                owner = record(binding.type);
                if (!owner)
                    fail("field path traverses a non-record value");
                name = identifier();
            }
            if (!scalar(binding.type))
                fail("view value binding must resolve a scalar");
        }
        if (binding.expression.rfind("snapshot.", 0) == 0)
            binding.identity = "model." + binding.expression.substr(9);
        else
            binding.identity = "getter." + binding.expression.substr(7, binding.expression.size() - 9);
        if (binding.identity.size() > 64)
            fail("resolved binding identity exceeds 64 bytes");
        return binding;
    }
    Action action()
    {
        word("model");
        take("dot");
        Action result;
        result.method = identifier();
        const auto *declaration = method(result.method);
        if (!declaration || declaration->at("pure").get<bool>())
            fail("event requires a public effect method");
        result.asynchronous = declaration->at("async").get<bool>();
        const auto returnType = declaration->at("returnType").get<std::string>();
        if (result.asynchronous && (returnType.rfind("Task<Result<", 0) != 0
            || returnType.size() < 21 || returnType.substr(returnType.size() - 9) != ",string>>"))
            fail("async event action must return Task<Result<T,string>>");
        if (!result.asynchronous && returnType.rfind("Task<", 0) == 0)
            fail("synchronous action cannot return a Task");
        std::size_t payloads = 0;
        for (const auto &parameter : declaration->at("parameters")) {
            const auto type = parameter.at("type").get<std::string>();
            if (type == "CancellationToken") {
                if (result.cancellation)
                    fail("action has multiple cancellation tokens");
                result.cancellation = true;
                result.cancellationParameters.push_back(true);
            } else {
                if (++payloads > 1 || !scalar(type))
                    fail("action requires at most one scalar event payload");
                result.payloadType = type;
                result.cancellationParameters.push_back(false);
            }
        }
        return result;
    }
    Node element(std::size_t depth)
    {
        if (depth > 8 || ++nodes_ > 128)
            fail("view node or depth capacity exceeded");
        Node node;
        node.kind = identifier();
        static const std::set<std::string> kinds = {"Column", "Row", "Label", "Button", "Toggle", "TextField", "PercentStepper"};
        if (!kinds.count(node.kind))
            fail("unsupported control kind");
        const bool container = node.kind == "Column" || node.kind == "Row";
        take("lparen");
        std::set<std::string> properties;
        std::string eventProperty;
        while (peek().kind != "rparen") {
            const auto property = identifier();
            if (!properties.insert(property).second)
                fail("duplicate control property");
            take("colon");
            if (property == "id") {
                node.identity = take("text").literal;
                if (node.identity.empty() || node.identity.size() > 64)
                    fail("invalid control identity");
                for (char character : node.identity) {
                    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                        || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.'))
                        fail("invalid control identity");
                }
            } else if (property == "label" || property == "text") {
                if (container || node.kind == "TextField")
                    fail("text property is inactive for this control");
                if (properties.count(property == "label" ? "text" : "label"))
                    fail("duplicate label/text property");
                node.label = take("text").literal;
                if (node.label.size() > 4096)
                    fail("control text byte limit exceeded");
            } else if (property == "sensitive") {
                if (container)
                    fail("sensitive is inactive on layout containers");
                node.sensitive = take("boolean").literal == "true";
            } else if (property == "value") {
                if (container || node.kind == "Button")
                    fail("value property is inactive for this control");
                node.value = read();
            } else if (property == "enabled") {
                if (container)
                    fail("enabled is inactive on layout containers");
                if (peek().kind == "boolean") {
                    node.enabledExpression = take("boolean").literal;
                } else {
                    const auto enabled = read();
                    if (enabled.type != "bool")
                        fail("enabled binding requires bool");
                    node.enabledExpression = enabled.expression;
                }
            } else if (property == "changed" || property == "clicked") {
                const bool editControl = node.kind == "Toggle" || node.kind == "TextField" || node.kind == "PercentStepper";
                if ((property == "clicked" && node.kind != "Button") || (property == "changed" && !editControl))
                    fail("event property is inactive for this control");
                eventProperty = property;
                node.action = action();
            } else if (property == "width" || property == "height") {
                const auto literal = take("integer").literal;
                int number = 0;
                const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size() || number < 0 || number > 8192)
                    fail("control dimension outside 0..8192");
                if (property == "width")
                    node.width = number;
                else
                    node.height = number;
            } else {
                fail("unknown control property: " + property);
            }
            if (peek().kind != "comma")
                break;
            take("comma");
            if (peek().kind == "rparen")
                fail("trailing property comma is unsupported");
        }
        take("rparen");
        if (node.identity.empty() || !identities_.insert(node.identity).second)
            fail("control identities must be explicit and unique");
        const bool editable = node.kind == "Toggle" || node.kind == "TextField" || node.kind == "PercentStepper";
        const auto requiredType = node.kind == "Toggle" ? "bool" : node.kind == "TextField" ? "string" : "int";
        if (editable && (node.value.expression.empty() || node.value.type != requiredType))
            fail("editable control value binding has the wrong scalar type");
        if (node.kind == "Button") {
            if (eventProperty != "clicked" || !node.action.payloadType.empty())
                fail("button requires clicked action with no event payload");
        } else if (editable) {
            if (eventProperty != "changed" || node.action.payloadType != requiredType)
                fail("editable control requires a matching typed changed action");
        } else if (!node.action.method.empty()) {
            fail("control kind does not emit action events");
        }
        if (container) {
            take("lbrace");
            while (peek().kind != "rbrace")
                node.children.push_back(element(depth + 1));
            take("rbrace");
            if (peek().kind == "semicolon")
                take("semicolon");
        } else {
            take("semicolon");
        }
        return node;
    }
};
bool validType(const std::string &type, const std::set<std::string> &names, unsigned depth = 0)
{
    if (depth > 8 || type.size() > 256)
        return false;
    if (scalar(type) || type == "void" || type == "CancellationToken" || names.count(type))
        return true;
    if (type.rfind("Task<", 0) == 0 && type.back() == '>')
        return validType(type.substr(5, type.size() - 6), names, depth + 1);
    if (type.rfind("Result<", 0) == 0 && type.size() > 15 && type.substr(type.size() - 8) == ",string>")
        return validType(type.substr(7, type.size() - 15), names, depth + 1);
    return false;
}
void validateManifest(const Json &manifest)
{
    members(manifest, {"protocol", "namespace", "records", "enums", "interfaces", "classes"});
    if (text(manifest, "protocol") != "pipelang.native-model-manifest.v1" || !safeNamespace(text(manifest, "namespace")))
        throw std::runtime_error("unsupported or unsafe model manifest");
    std::set<std::string> types;
    for (const auto *category : {"records", "enums", "interfaces", "classes"}) {
        boundedArray(manifest.at(category));
        for (const auto &item : manifest.at(category)) {
            const auto name = text(item, "name");
            if (!safeName(name) || !types.insert(name).second)
                throw std::runtime_error("unsafe or duplicate manifest type");
        }
    }
    for (const auto *category : {"records", "enums", "interfaces", "classes"}) {
        boundedArray(manifest.at(category));
        for (const auto &item : manifest.at(category)) {
            const std::string group(category);
            std::set<std::string> memberNames;
            if (group == "records" || group == "classes") {
                members(item, group == "records" ? std::set<std::string>{"name", "fields"}
                    : std::set<std::string>{"name", "fields", "methods", "constructor"});
                boundedArray(item.at("fields"));
                for (const auto &field : item.at("fields")) {
                    members(field, {"name", "type"});
                    if (!safeName(text(field, "name")) || !memberNames.insert(text(field, "name")).second)
                        throw std::runtime_error("unsafe or duplicate manifest field");
                    if (!validType(text(field, "type"), types))
                        throw std::runtime_error("unknown manifest field type");
                }
            }
            if (group == "classes") {
                const auto &constructor = item.at("constructor");
                members(constructor, {"parameters"});
                boundedArray(constructor.at("parameters"));
                std::set<std::string> parameters;
                for (const auto &parameter : constructor.at("parameters")) {
                    members(parameter, {"name", "type"});
                    if (!safeName(text(parameter, "name")) || !parameters.insert(text(parameter, "name")).second)
                        throw std::runtime_error("unsafe or duplicate manifest constructor parameter");
                    if (!validType(text(parameter, "type"), types) || text(parameter, "type") == "void")
                        throw std::runtime_error("unknown manifest constructor parameter type");
                }
            }
            if (group == "classes" || group == "interfaces") {
                if (group == "interfaces")
                    members(item, {"name", "methods"});
                boundedArray(item.at("methods"));
                for (const auto &method : item.at("methods")) {
                    members(method, {"name", "returnType", "parameters", "async", "pure"});
                    if (!safeName(text(method, "name")) || !memberNames.insert(text(method, "name")).second
                        || !method.at("async").is_boolean() || !method.at("pure").is_boolean())
                        throw std::runtime_error("unsafe, duplicate or malformed manifest method");
                    if (!validType(text(method, "returnType"), types))
                        throw std::runtime_error("unknown manifest return type");
                    boundedArray(method.at("parameters"));
                    std::set<std::string> parameters;
                    for (const auto &parameter : method.at("parameters")) {
                        members(parameter, {"name", "type"});
                        if (!safeName(text(parameter, "name")) || !parameters.insert(text(parameter, "name")).second)
                            throw std::runtime_error("unsafe or duplicate manifest parameter");
                        if (!validType(text(parameter, "type"), types) || text(parameter, "type") == "void")
                            throw std::runtime_error("unknown manifest parameter type");
                    }
                }
            }
            if (group == "enums") {
                members(item, {"name", "values"});
                boundedArray(item.at("values"));
                std::set<std::string> values;
                for (const auto &value : item.at("values")) {
                    if (!value.is_string() || !safeName(value.get<std::string>()) || !values.insert(value.get<std::string>()).second)
                        throw std::runtime_error("unsafe or duplicate enum value");
                }
            }
        }
    }
}
}
Json decode(const std::string &input)
{
    std::vector<std::set<std::string>> keys;
    std::size_t nodes = 0;
    return Json::parse(input, [&](int depth, Json::parse_event_t event, Json &value) {
        using Event = Json::parse_event_t;
        if (depth > 24 || ++nodes > 50000)
            throw std::runtime_error("request JSON capacity exceeded");
        if (event == Event::object_start)
            keys.emplace_back();
        if (event == Event::key && (keys.empty() || !keys.back().insert(value.get<std::string>()).second))
            throw std::runtime_error("duplicate JSON member");
        if (event == Event::object_end)
            keys.pop_back();
        return true;
    });
}
Json compile(const Json &request)
{
    try {
        members(request, {"protocol", "source", "model_manifest", "model_header"}, {"namespace", "style_inspection"});
        const bool inspectStyle = request.contains("style_inspection");
        if (inspectStyle && text(request, "style_inspection") != styleDescriptorProtocol)
            throw std::runtime_error("unsupported style inspection protocol");
        if (text(request, "protocol") != "pipelang.native-model-view.v1")
            throw std::runtime_error("unsupported model-view protocol");
        const auto source = text(request, "source");
        validateManifest(request.at("model_manifest"));
        const auto nameSpace = request.contains("namespace") ? text(request, "namespace") : "pipelang_models";
        if (!safeNamespace(nameSpace) || nameSpace != request.at("model_manifest").at("namespace"))
            throw std::runtime_error("namespace must safely match model manifest");
        const auto header = text(request, "model_header");
        if (header.empty() || header.size() > 256 || header.front() == '/' || header.find("..") != std::string::npos)
            throw std::runtime_error("model_header must be a safe relative include");
        for (char character : header) {
            if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.' || character == '/'))
                throw std::runtime_error("model_header contains unsafe characters");
        }
        const auto lexed = pipelang::native::lex(source);
        if (lexed.failure)
            throw Failure{lexed.failure->message, lexed.failure->start, lexed.failure->end};
        Parser parser(lexed.tokens, request.at("model_manifest"));
        const auto view = parser.parse(nameSpace, header);
        Json response = {{"protocol", "pipelang.native-model-view.v1"}, {"ok", true}, {"header", emit(view)},
            {"manifest", {{"protocol", "pipelang.native-model-view-manifest.v1"}, {"name", view.name},
                {"adapter", view.name + "Adapter"}, {"model", view.model}, {"namespace", nameSpace}}}};
        if (inspectStyle)
            response["style_descriptor"] = styleDescriptor(view);
        return response;
    } catch (const styles::Failure &failure) {
        return {{"protocol", "pipelang.native-model-view.v1"}, {"ok", false},
            {"diagnostics", Json::array({{{"code", "PMV2001"}, {"message", failure.message},
                {"start", failure.start}, {"end", failure.end}}})}};
    } catch (const Failure &failure) {
        return {{"protocol", "pipelang.native-model-view.v1"}, {"ok", false},
            {"diagnostics", Json::array({{{"code", "PMV1001"}, {"message", failure.message},
                {"start", failure.start}, {"end", failure.end}}})}};
    }
}
}
