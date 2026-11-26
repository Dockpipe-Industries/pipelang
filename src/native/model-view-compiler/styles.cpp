// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "styles.hpp"
#include <charconv>
#include <cmath>
#include <set>
#include <utility>

namespace pipelang::model_view::styles {
namespace {
using Token = native::Token;
struct PropertyRule {
    ValueKind kind;
    double minimum;
    double maximum;
};
const std::map<std::string, PropertyRule> propertyRules = {
    {"padding", {ValueKind::Length, 0, 64}},
    {"gap", {ValueKind::Length, 0, 64}},
    {"labelHeight", {ValueKind::Length, 1, 128}},
    {"controlHeight", {ValueKind::Length, 1, 256}},
    {"fontPixelHeight", {ValueKind::Length, 7, 56}},
    {"textInset", {ValueKind::Length, 0, 64}},
    {"borderWidth", {ValueKind::Length, 0, 8}},
    {"background", {ValueKind::Color, 0, 0}},
    {"control", {ValueKind::Color, 0, 0}},
    {"field", {ValueKind::Color, 0, 0}},
    {"border", {ValueKind::Color, 0, 0}},
    {"focus", {ValueKind::Color, 0, 0}},
    {"text", {ValueKind::Color, 0, 0}},
    {"disabledText", {ValueKind::Color, 0, 0}}
};
[[noreturn]] void fail(const Token &token, const std::string &message)
{
    throw Failure{message, token.start, token.end};
}
const Token &current(const std::vector<Token> &tokens, std::size_t index)
{
    if (index >= tokens.size())
        throw Failure{"unexpected end of style source", 0, 0};
    return tokens[index];
}
bool word(const Token &token, const std::string &literal)
{
    return token.kind == "identifier" && token.literal == literal;
}
struct Expression {
    Token location;
    std::string reference;
    Value value;
};
struct Declaration {
    Token name;
    ValueKind kind;
    bool overriding = false;
    Expression expression;
};
struct Style {
    Token name;
    std::optional<Token> parent;
    std::map<std::string, Declaration> tokens;
    std::map<std::string, Declaration> properties;
};
struct EffectiveStyle {
    std::map<std::string, Declaration> tokens;
    std::map<std::string, Declaration> properties;
};
class Parser {
public:
    Parser(const std::vector<Token> &tokens, std::size_t &index)
        : tokens_(tokens), index_(index) {}

    std::map<std::string, Style> parseDeclarations()
    {
        std::map<std::string, Style> declarations;
        while (word(peek(), "style")) {
            ++index_;
            Style style;
            style.name = take("identifier");
            if (declarations.size() >= 32)
                fail(style.name, "style declaration limit is 32");
            if (declarations.count(style.name.literal))
                fail(style.name, "duplicate style name");
            if (word(peek(), "extends")) {
                ++index_;
                style.parent = take("identifier");
            }
            take("lbrace");
            while (peek().kind != "rbrace")
                parseMember(style);
            take("rbrace");
            declarations.emplace(style.name.literal, std::move(style));
        }
        return declarations;
    }
private:
    const std::vector<Token> &tokens_;
    std::size_t &index_;

    const Token &peek() const { return current(tokens_, index_); }
    Token take(const std::string &kind)
    {
        const auto token = peek();
        if (token.kind != kind)
            fail(token, "expected " + kind + " in style declaration; selectors, parts, states and nesting are unsupported");
        ++index_;
        return token;
    }
    Expression expression()
    {
        Expression result;
        result.location = peek();
        if (peek().kind == "identifier") {
            result.reference = take("identifier").literal;
        } else if (peek().kind == "integer" || peek().kind == "float") {
            const auto number = peek();
            ++index_;
            double value = 0;
            const auto converted = std::from_chars(number.literal.data(),
                number.literal.data() + number.literal.size(), value, std::chars_format::fixed);
            if (converted.ec != std::errc{} || converted.ptr != number.literal.data() + number.literal.size()
                || !std::isfinite(value))
                fail(number, "style length must be a finite bounded decimal");
            const auto unit = take("identifier");
            if (unit.literal != "px")
                fail(unit, "style lengths require logical px units");
            result.value.kind = ValueKind::Length;
            result.value.length = value;
        } else if (peek().kind == "text") {
            const auto color = take("text");
            if (color.literal.size() != 9 || color.literal.front() != '#')
                fail(color, "style colors require exactly #RRGGBBAA");
            for (std::size_t position = 1; position < color.literal.size(); ++position) {
                const char digit = color.literal[position];
                if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')
                    || (digit >= 'A' && digit <= 'F')))
                    fail(color, "style colors require exactly #RRGGBBAA");
            }
            result.value.kind = ValueKind::Color;
            result.value.color = color.literal;
        } else {
            fail(peek(), "expected a typed style literal or token reference");
        }
        return result;
    }
    void parseMember(Style &style)
    {
        Declaration declaration;
        if (word(peek(), "override")) {
            declaration.overriding = true;
            ++index_;
        }
        const bool isToken = word(peek(), "token");
        if (isToken)
            ++index_;
        declaration.name = take("identifier");
        take("colon");
        if (isToken) {
            const auto type = take("identifier");
            if (type.literal != "length" && type.literal != "color")
                fail(type, "style token type must be length or color");
            declaration.kind = type.literal == "length" ? ValueKind::Length : ValueKind::Color;
            take("assign");
        } else {
            const auto rule = propertyRules.find(declaration.name.literal);
            if (rule == propertyRules.end())
                fail(declaration.name, "unknown or unsupported style property: " + declaration.name.literal);
            declaration.kind = rule->second.kind;
        }
        declaration.expression = expression();
        take("semicolon");
        auto &members = isToken ? style.tokens : style.properties;
        if (members.count(declaration.name.literal))
            fail(declaration.name, "duplicate style member");
        if (isToken && members.size() >= 32)
            fail(declaration.name, "style token limit is 32");
        members.emplace(declaration.name.literal, std::move(declaration));
    }
};

class Resolver {
public:
    explicit Resolver(const std::map<std::string, Style> &styles) : styles_(styles) {}
    StyleSet resolveAll()
    {
        StyleSet result;
        for (const auto &[name, style] : styles_) {
            std::set<std::string> ancestors;
            const auto effective = inherit(style, ancestors, 1);
            ResolvedStyle resolved;
            resolved.name = name;
            // Inherited expressions bind against the child's effective token scope.
            // Resolve afresh so overrides cannot reuse values from a parent's scope.
            // Check every effective token, even if no property references it.
            for (const auto &[tokenName, token] : effective.tokens) {
                std::set<std::string> visiting;
                resolveToken(tokenName, effective, visiting, 1, token.name);
            }
            for (const auto &[propertyName, property] : effective.properties) {
                std::set<std::string> visiting;
                const auto value = resolveExpression(property.expression, effective, visiting, 0);
                checkType(property, value);
                const auto &rule = propertyRules.at(propertyName);
                if (value.kind == ValueKind::Length
                    && (value.length < rule.minimum || value.length > rule.maximum))
                    fail(property.expression.location, "style property length is outside its supported range: " + propertyName);
                resolved.properties.emplace(propertyName, value);
            }
            result.emplace(name, std::move(resolved));
        }
        return result;
    }
private:
    const std::map<std::string, Style> &styles_;
    std::size_t work_ = 0;
    void work(const Token &location)
    {
        if (++work_ > 4096)
            fail(location, "style resolution work limit is 4096");
    }
    static void checkType(const Declaration &declaration, const Value &value)
    {
        if (declaration.kind != value.kind)
            fail(declaration.expression.location, "style value type does not match declaration: " + declaration.name.literal);
    }
    void merge(std::map<std::string, Declaration> &inherited,
               const std::map<std::string, Declaration> &local)
    {
        for (const auto &[name, declaration] : local) {
            work(declaration.name);
            const auto existing = inherited.find(name);
            if (declaration.overriding && existing == inherited.end())
                fail(declaration.name, "override requires an inherited member: " + name);
            if (!declaration.overriding && existing != inherited.end())
                fail(declaration.name, "inherited member replacement requires override: " + name);
            if (existing != inherited.end() && existing->second.kind != declaration.kind)
                fail(declaration.name, "override must preserve the inherited token type: " + name);
            inherited.insert_or_assign(name, declaration);
        }
    }
    EffectiveStyle inherit(const Style &style, std::set<std::string> &ancestors, std::size_t depth)
    {
        work(style.name);
        if (depth > 16)
            fail(style.name, "style inheritance depth limit is 16");
        if (!ancestors.insert(style.name.literal).second)
            fail(style.name, "style inheritance cycle");
        EffectiveStyle effective;
        if (style.parent) {
            const auto parent = styles_.find(style.parent->literal);
            if (parent == styles_.end())
                fail(*style.parent, "unknown inherited style: " + style.parent->literal);
            effective = inherit(parent->second, ancestors, depth + 1);
        }
        merge(effective.tokens, style.tokens);
        merge(effective.properties, style.properties);
        if (effective.tokens.size() > 32)
            fail(style.name, "effective style token limit is 32");
        ancestors.erase(style.name.literal);
        return effective;
    }
    Value resolveExpression(const Expression &expression, const EffectiveStyle &style,
                            std::set<std::string> &visiting, std::size_t depth)
    {
        work(expression.location);
        if (expression.reference.empty())
            return expression.value;
        return resolveToken(expression.reference, style, visiting, depth + 1, expression.location);
    }
    Value resolveToken(const std::string &name, const EffectiveStyle &style,
                       std::set<std::string> &visiting, std::size_t depth, const Token &location)
    {
        work(location);
        if (depth > 16)
            fail(location, "style token reference depth limit is 16");
        const auto declaration = style.tokens.find(name);
        if (declaration == style.tokens.end())
            fail(location, "unknown style token: " + name);
        if (!visiting.insert(name).second)
            fail(location, "style token reference cycle: " + name);
        const auto value = resolveExpression(declaration->second.expression, style, visiting, depth);
        checkType(declaration->second, value);
        visiting.erase(name);
        return value;
    }
};
}

StyleSet parse(const std::vector<native::Token> &tokens, std::size_t &index)
{
    const auto declarations = Parser(tokens, index).parseDeclarations();
    return Resolver(declarations).resolveAll();
}

std::optional<ResolvedStyle> selection(const StyleSet &styles,
                                     const std::vector<native::Token> &tokens,
                                     std::size_t &index)
{
    if (!word(current(tokens, index), "style"))
        return std::nullopt;
    ++index;
    const auto &name = current(tokens, index);
    if (name.kind != "identifier")
        fail(name, "expected style name after style selection");
    const auto found = styles.find(name.literal);
    if (found == styles.end())
        fail(name, "unknown selected style: " + name.literal);
    ++index;
    return found->second;
}
}
