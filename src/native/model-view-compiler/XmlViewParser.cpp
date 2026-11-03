// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "XmlViewParser.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace pipelang::model_view {
namespace {

bool xmlCharacter(std::uint32_t value)
{
    return value == 9 || value == 10 || value == 13
        || (value >= 0x20 && value <= 0xD7FF)
        || (value >= 0xE000 && value <= 0xFFFD)
        || (value >= 0x10000 && value <= 0x10FFFF);
}

void appendUtf8(std::string &text, std::uint32_t value)
{
    if (value <= 0x7F) {
        text.push_back(static_cast<char>(value));
    } else if (value <= 0x7FF) {
        text.push_back(static_cast<char>(0xC0 | (value >> 6)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value <= 0xFFFF) {
        text.push_back(static_cast<char>(0xE0 | (value >> 12)));
        text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (value >> 18)));
        text.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

bool nameStart(char character)
{
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z') || character == '_';
}

}

XmlViewParser::XmlViewParser(const std::string &source) : source_(source) {}

[[noreturn]] void XmlViewParser::fail(const std::string &message, std::size_t start, std::size_t end) const
{
    throw native::LexicalFailure{"PMV3001", message, start, end};
}

bool XmlViewParser::starts(const std::string &text) const
{
    return source_.compare(position_, text.size(), text) == 0;
}

void XmlViewParser::expect(const std::string &text)
{
    if (!starts(text))
        fail("expected XML " + text, position_, std::min(position_ + 1, source_.size()));
    position_ += text.size();
}

bool XmlViewParser::whitespace()
{
    const auto start = position_;
    while (position_ < source_.size()) {
        const auto character = source_[position_];
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
            break;
        ++position_;
    }
    return position_ != start;
}

void XmlViewParser::trivia()
{
    whitespace();
    while (starts("<!--")) {
        const auto start = position_;
        const auto end = source_.find("--", position_ + 4);
        if (end == std::string::npos || source_.compare(end, 3, "-->") != 0)
            fail("malformed XML comment", start, std::min(start + 4, source_.size()));
        position_ = end + 3;
        whitespace();
    }
    if (starts("<!") || starts("<?"))
        fail("XML DTDs, CDATA and processing instructions are unsupported", position_,
             std::min(position_ + 2, source_.size()));
}

std::string XmlViewParser::name()
{
    const auto start = position_;
    if (position_ == source_.size() || !nameStart(source_[position_]))
        fail("expected an unqualified ASCII XML name", start, std::min(start + 1, source_.size()));
    ++position_;
    while (position_ < source_.size()) {
        const auto character = source_[position_];
        if (!nameStart(character) && !(character >= '0' && character <= '9') && character != '-')
            break;
        ++position_;
    }
    return source_.substr(start, position_ - start);
}

XmlViewParser::Attribute XmlViewParser::attribute()
{
    Attribute result;
    result.start = position_;
    result.name = name();
    whitespace();
    expect("=");
    whitespace();
    if (position_ == source_.size() || (source_[position_] != '"' && source_[position_] != '\''))
        fail("XML attribute values must be quoted", result.start, position_);
    const auto quote = source_[position_++];
    while (position_ < source_.size() && source_[position_] != quote) {
        const auto start = position_;
        const auto character = source_[position_++];
        if (character == '<')
            fail("raw '<' is forbidden in XML attributes", start, position_);
        if (character == '&') {
            const auto end = source_.find(';', position_);
            if (end == std::string::npos || end - position_ > 16)
                fail("unterminated or oversized XML character reference", start, position_);
            const auto entity = source_.substr(position_, end - position_);
            if (entity == "amp") {
                result.value += '&';
            } else if (entity == "lt") {
                result.value += '<';
            } else if (entity == "gt") {
                result.value += '>';
            } else if (entity == "quot") {
                result.value += '"';
            } else if (entity == "apos") {
                result.value += '\'';
            } else if (!entity.empty() && entity.front() == '#') {
                const bool hexadecimal = entity.size() > 1 && entity[1] == 'x';
                const auto digits = std::string_view(entity).substr(hexadecimal ? 2 : 1);
                std::uint32_t value = 0;
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value,
                                                    hexadecimal ? 16 : 10);
                if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()
                    || !xmlCharacter(value))
                    fail("invalid XML character reference", start, end + 1);
                appendUtf8(result.value, value);
            } else {
                fail("unknown XML entity; entity declarations are unsupported", start, end + 1);
            }
            position_ = end + 1;
        } else if (character == '\r' || character == '\n' || character == '\t') {
            if (character == '\r' && starts("\n"))
                ++position_;
            result.value += ' ';
        } else {
            result.value += character;
        }
    }
    if (position_ == source_.size())
        fail("unterminated XML attribute", result.start, position_);
    ++position_;
    result.end = position_;
    return result;
}

XmlViewParser::OpeningTag XmlViewParser::openingTag(bool declaration)
{
    OpeningTag result;
    result.start = position_;
    expect(declaration ? "<?" : "<");
    result.name = name();
    std::set<std::string> names;
    while (true) {
        const bool separated = whitespace();
        if (declaration && starts("?>")) {
            position_ += 2;
            break;
        }
        if (!declaration && starts("/>")) {
            position_ += 2;
            result.empty = true;
            break;
        }
        if (!declaration && starts(">")) {
            ++position_;
            break;
        }
        if (!separated)
            fail("XML attributes require whitespace separation", position_, std::min(position_ + 1, source_.size()));
        auto value = attribute();
        if (!names.insert(value.name).second)
            fail("duplicate XML attribute: " + value.name, value.start, value.end);
        if (result.attributes.size() >= 16)
            fail("XML attribute capacity is 16 per element", value.start, value.end);
        result.attributes.push_back(std::move(value));
    }
    result.end = position_;
    return result;
}

void XmlViewParser::closingTag(const OpeningTag &opening)
{
    trivia();
    const auto start = position_;
    expect("</");
    if (name() != opening.name)
        fail("mismatched XML closing tag for " + opening.name, start, position_);
    whitespace();
    expect(">");
}

void XmlViewParser::validateCharacters() const
{
    for (std::size_t index = 0; index < source_.size();) {
        const auto start = index;
        const auto first = static_cast<unsigned char>(source_[index++]);
        std::uint32_t value = first;
        unsigned continuation = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            value = first & 0x1F;
            continuation = 1;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            value = first & 0x0F;
            continuation = 2;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            value = first & 0x07;
            continuation = 3;
            minimum = 0x10000;
        } else if (first >= 0x80) {
            fail("invalid UTF-8 in XML source", start, index);
        }
        for (unsigned count = 0; count < continuation; ++count) {
            if (index == source_.size() || (static_cast<unsigned char>(source_[index]) & 0xC0) != 0x80)
                fail("invalid UTF-8 in XML source", start, index);
            value = (value << 6) | (static_cast<unsigned char>(source_[index++]) & 0x3F);
        }
        if (value < minimum || !xmlCharacter(value))
            fail("invalid XML character or UTF-8 scalar", start, index);
    }
}

const XmlViewParser::Attribute *XmlViewParser::find(const OpeningTag &tag, const std::string &name) const
{
    for (const auto &attribute : tag.attributes) {
        if (attribute.name == name)
            return &attribute;
    }
    return nullptr;
}

const XmlViewParser::Attribute &XmlViewParser::required(const OpeningTag &tag, const std::string &name) const
{
    const auto *attribute = find(tag, name);
    if (!attribute)
        fail("missing XML attribute: " + name, tag.start, tag.end);
    return *attribute;
}

void XmlViewParser::attributes(const OpeningTag &tag, std::initializer_list<const char *> requiredNames,
                               std::initializer_list<const char *> optional) const
{
    for (const auto *name : requiredNames)
        required(tag, name);
    for (const auto &attribute : tag.attributes) {
        const auto matches = [&](const char *name) { return attribute.name == name; };
        if (std::none_of(requiredNames.begin(), requiredNames.end(), matches)
            && std::none_of(optional.begin(), optional.end(), matches))
            fail("unknown XML attribute: " + attribute.name, attribute.start, attribute.end);
    }
}

void XmlViewParser::declaration()
{
    const auto tag = openingTag(true);
    if (tag.name != "xml")
        fail("only an XML declaration is allowed before View", tag.start, tag.end);
    attributes(tag, {"version"}, {"encoding", "standalone"});
    for (const auto &attribute : tag.attributes) {
        const auto raw = source_.substr(attribute.start, attribute.end - attribute.start);
        if (raw.find('&') != std::string::npos)
            fail("XML declaration values cannot contain character references", attribute.start, attribute.end);
    }
    if (tag.attributes.front().name != "version" || required(tag, "version").value != "1.0")
        fail("PipeView XML requires XML version 1.0", tag.start, tag.end);
    if (const auto *encoding = find(tag, "encoding")) {
        if (encoding->value != "UTF-8" && encoding->value != "utf-8")
            fail("PipeView XML requires UTF-8 encoding", encoding->start, encoding->end);
        if (tag.attributes[1].name != "encoding")
            fail("XML encoding must precede standalone", encoding->start, encoding->end);
    }
    if (const auto *standalone = find(tag, "standalone")) {
        if (standalone->value != "yes" && standalone->value != "no")
            fail("XML standalone must be yes or no", standalone->start, standalone->end);
    }
}

void XmlViewParser::token(const std::string &kind, const std::string &literal,
                          std::size_t start, std::size_t end)
{
    if (tokens_.size() >= 16384)
        fail("XML projected token capacity is 16384", start, end);
    tokens_.push_back({kind, literal, start, end});
}

void XmlViewParser::token(const std::string &kind, const std::string &literal, const OpeningTag &tag)
{
    token(kind, literal, tag.start, tag.end);
}

void XmlViewParser::token(const std::string &kind, const std::string &literal, const Attribute &attribute)
{
    token(kind, literal, attribute.start, attribute.end);
}

std::vector<native::Token> XmlViewParser::expression(const Attribute &attribute, bool braced) const
{
    auto value = attribute.value;
    if (braced) {
        if (value.size() < 2 || value.front() != '{' || value.back() != '}')
            fail("XML model references require braces", attribute.start, attribute.end);
        value = value.substr(1, value.size() - 2);
    }
    const auto lexed = native::lex(value);
    if (lexed.failure)
        fail("invalid XML attribute expression: " + lexed.failure->message, attribute.start, attribute.end);
    auto result = lexed.tokens;
    result.pop_back();
    for (auto &item : result) {
        item.start = attribute.start;
        item.end = attribute.end;
    }
    return result;
}

void XmlViewParser::identifier(const Attribute &attribute)
{
    const auto items = expression(attribute, false);
    if (items.size() != 1 || items.front().kind != "identifier" || items.front().literal != attribute.value)
        fail("XML attribute requires one identifier", attribute.start, attribute.end);
    token("identifier", items.front().literal, attribute);
}

void XmlViewParser::reference(const Attribute &attribute, bool binding)
{
    const auto items = expression(attribute, true);
    std::size_t index = 0;
    const auto take = [&](const std::string &kind, const std::string &literal = "") {
        if (index >= items.size() || items[index].kind != kind
            || (!literal.empty() && items[index].literal != literal))
            fail("invalid XML typed model reference", attribute.start, attribute.end);
        ++index;
    };
    if (binding)
        take("identifier", "bind");
    take("identifier", "model");
    take("dot");
    take("identifier");
    if (binding) {
        if (index < items.size() && items[index].kind == "lparen") {
            take("lparen");
            take("rparen");
        } else {
            while (index < items.size() && items[index].kind == "dot") {
                take("dot");
                take("identifier");
            }
        }
    }
    if (index != items.size())
        fail("XML model reference has unexpected trailing tokens", attribute.start, attribute.end);
    for (const auto &item : items)
        token(item.kind, item.literal, attribute);
}

void XmlViewParser::property(const Attribute &attribute)
{
    token("identifier", attribute.name, attribute);
    token("colon", ":", attribute);
    if (attribute.name == "id" || attribute.name == "label" || attribute.name == "text") {
        token("text", attribute.value, attribute);
    } else if (attribute.name == "value") {
        reference(attribute, true);
    } else if (attribute.name == "changed" || attribute.name == "clicked") {
        reference(attribute, false);
    } else if (attribute.name == "enabled" && attribute.value != "true" && attribute.value != "false") {
        reference(attribute, true);
    } else if (attribute.name == "enabled" || attribute.name == "sensitive") {
        if (attribute.value != "true" && attribute.value != "false")
            fail("XML Boolean attribute requires true or false", attribute.start, attribute.end);
        token("boolean", attribute.value, attribute);
    } else {
        if (attribute.value.empty() || attribute.value.find_first_not_of("0123456789") != std::string::npos)
            fail("XML dimension requires a nonnegative decimal integer", attribute.start, attribute.end);
        token("integer", attribute.value, attribute);
    }
}

void XmlViewParser::element(const OpeningTag &tag, std::size_t depth)
{
    if (depth > 8 || ++nodes_ > 128)
        fail("view node or depth capacity exceeded", tag.start, tag.end);
    if (tag.name == "State" || tag.name == "Action")
        fail("historical XML State/Action declarations are unsupported; bind an external typed model", tag.start, tag.end);
    static const std::set<std::string> kinds = {"Column", "Row", "Label", "Button", "Toggle", "TextField", "PercentStepper"};
    if (!kinds.count(tag.name))
        fail("unsupported XML control kind: " + tag.name, tag.start, tag.end);
    attributes(tag, {"id"}, {"label", "text", "value", "enabled", "sensitive", "width", "height", "changed", "clicked"});
    token("identifier", tag.name, tag);
    token("lparen", "(", tag);
    for (std::size_t index = 0; index < tag.attributes.size(); ++index) {
        if (index != 0)
            token("comma", ",", tag.attributes[index - 1]);
        property(tag.attributes[index]);
    }
    token("rparen", ")", tag.attributes.back());
    const bool container = tag.name == "Column" || tag.name == "Row";
    if (container)
        token("lbrace", "{", tag);
    if (!tag.empty) {
        trivia();
        while (!starts("</")) {
            if (!container)
                fail("XML leaf controls cannot contain text or child elements", position_, std::min(position_ + 1, source_.size()));
            element(openingTag(), depth + 1);
            trivia();
        }
        closingTag(tag);
    }
    if (container)
        token("rbrace", "}", tag);
    else
        token("semicolon", ";", tag);
}

void XmlViewParser::styleMember(const OpeningTag &tag)
{
    const bool isToken = tag.name == "Token";
    if (!isToken && tag.name != "Property")
        fail("XML Style children must be Token or Property", tag.start, tag.end);
    if (isToken)
        attributes(tag, {"name", "type", "value"}, {"override"});
    else
        attributes(tag, {"name", "value"}, {"override"});
    if (const auto *overriding = find(tag, "override")) {
        if (overriding->value != "true" && overriding->value != "false")
            fail("XML override requires true or false", overriding->start, overriding->end);
        if (overriding->value == "true")
            token("identifier", "override", *overriding);
    }
    if (isToken)
        token("identifier", "token", tag);
    identifier(required(tag, "name"));
    token("colon", ":", tag);
    if (isToken) {
        identifier(required(tag, "type"));
        token("assign", "=", tag);
    }
    const auto &value = required(tag, "value");
    if (!value.value.empty() && value.value.front() == '#') {
        token("text", value.value, value);
    } else {
        const auto items = expression(value, false);
        const bool reference = items.size() == 1 && items.front().kind == "identifier";
        const bool length = items.size() == 2 && (items[0].kind == "integer" || items[0].kind == "float")
            && items[1].kind == "identifier";
        if (!reference && !length)
            fail("XML style value requires a color, length or token reference", value.start, value.end);
        for (const auto &item : items)
            token(item.kind, item.literal, value);
    }
    token("semicolon", ";", value);
    if (!tag.empty)
        closingTag(tag);
}

void XmlViewParser::style(const OpeningTag &tag)
{
    attributes(tag, {"name"}, {"extends"});
    token("identifier", "style", tag);
    identifier(required(tag, "name"));
    if (const auto *parent = find(tag, "extends")) {
        token("identifier", "extends", *parent);
        identifier(*parent);
    }
    token("lbrace", "{", tag);
    if (!tag.empty) {
        trivia();
        std::size_t members = 0;
        while (!starts("</")) {
            if (++members > 128)
                fail("XML style member capacity is 128", tag.start, tag.end);
            styleMember(openingTag());
            trivia();
        }
        closingTag(tag);
    }
    token("rbrace", "}", tag);
}

native::LexResult XmlViewParser::parse()
{
    try {
        if (source_.size() > 65536)
            fail("XML source byte limit is 65536", 0, 0);
        validateCharacters();
        if (starts("\xEF\xBB\xBF"))
            position_ += 3;
        if (starts("<?xml"))
            declaration();
        trivia();
        const auto view = openingTag();
        if (view.name != "View")
            fail("PipeView XML requires a View root", view.start, view.end);
        if (const auto *profile = find(view, "profile")) {
            if (profile->value != "pipelang.native-model-view.v1")
                fail("unsupported XML profile; historical State/Action views require an external typed model", profile->start, profile->end);
        }
        attributes(view, {"name", "model", "profile"}, {"style"});
        if (view.empty)
            fail("XML View requires exactly one control root", view.start, view.end);
        trivia();
        auto child = openingTag();
        std::size_t styles = 0;
        while (child.name == "Style") {
            if (++styles > 32)
                fail("style declaration limit is 32", child.start, child.end);
            style(child);
            trivia();
            child = openingTag();
        }
        token("identifier", "view", view);
        identifier(required(view, "name"));
        token("lparen", "(", view);
        token("identifier", "model", view);
        token("colon", ":", view);
        identifier(required(view, "model"));
        token("rparen", ")", view);
        if (const auto *selection = find(view, "style")) {
            token("identifier", "style", *selection);
            identifier(*selection);
        }
        token("lbrace", "{", view);
        element(child, 1);
        closingTag(view);
        token("rbrace", "}", view);
        trivia();
        if (position_ != source_.size())
            fail("unexpected XML content after View", position_, source_.size());
        token("eof", "", source_.size(), source_.size());
        return {std::move(tokens_), std::nullopt};
    } catch (const native::LexicalFailure &failure) {
        return {{}, failure};
    }
}

}
