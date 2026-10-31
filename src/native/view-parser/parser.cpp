#include "parser.hpp"

#include <charconv>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace pipelang::view {
namespace {

struct SyntaxFailure {
    Json value;
};

class Parser {
public:
    explicit Parser(const Request& request) : request_(request) {}

    Json declaration() {
        const auto start = take("identifier", "view");
        if (start.literal != "view")
            fail("PL2001", span(start), "expected view declaration");
        const auto name = take("identifier", "view name");
        take("lbrace", "'{' after view name");
        Json states = Json::array();
        Json events = Json::array();
        Json actions = Json::array();
        while (peek().kind == "identifier") {
            const auto& word = peek().literal;
            if (word == "state") {
                if (states.size() >= 128)
                    fail("PL2001", span(peek()), "view state declaration limit exceeded");
                states.push_back(state());
            } else if (word == "event") {
                if (events.size() >= 128)
                    fail("PL2001", span(peek()), "view event declaration limit exceeded");
                events.push_back(event());
            } else if (word == "action") {
                if (actions.size() >= 128)
                    fail("PL2001", span(peek()), "view action declaration limit exceeded");
                actions.push_back(action());
            } else {
                break;
            }
        }
        auto root = element(1);
        const auto end = take("rbrace", "'}' after the single root element");
        take("eof", "end of file");
        return {{"Name", name.literal}, {"Span", span(start, end)},
                {"State", std::move(states)}, {"Events", std::move(events)},
                {"Actions", std::move(actions)}, {"Root", std::move(root)}};
    }

private:
    const Request& request_;
    std::size_t index_ = 0;
    int elements_ = 0;

    const Token& peek() const {
        return request_.tokens.at(index_);
    }

    Token take(const std::string& kind, const std::string& description) {
        const auto current = peek();
        if (current.kind != kind)
            fail("PL2002", span(current), "expected " + description);
        if (kind != "eof")
            ++index_;
        return current;
    }

    [[noreturn]] void fail(const std::string& code, Json extent, const std::string& message) const {
        throw SyntaxFailure{{{"code", code}, {"message", message}, {"span", std::move(extent)}}};
    }

    Json span(const Token& first, const Token& last) const {
        return {{"file", request_.file}, {"start", first.start}, {"end", last.end}};
    }

    Json span(const Token& token) const {
        return span(token, token);
    }

    Json state() {
        const auto start = peek();
        ++index_;
        const auto name = take("identifier", "state name");
        take("colon", "':' after state name");
        const auto type = take("identifier", "state type");
        take("assign", "'=' before state initializer");
        auto initial = value(false);
        const auto end = take("semicolon", "';' after state declaration");
        return {{"Name", name.literal}, {"Type", type.literal},
                {"Value", std::move(initial)}, {"Span", span(start, end)}};
    }

    Json event() {
        const auto start = peek();
        ++index_;
        const auto name = take("identifier", "event name");
        take("lparen", "'(' after event name");
        std::string payload;
        if (peek().kind == "identifier") {
            payload = peek().literal;
            ++index_;
        }
        take("rparen", "')' after optional event payload type");
        const auto end = take("semicolon", "';' after event declaration");
        return {{"Name", name.literal}, {"Payload", payload}, {"Span", span(start, end)}};
    }

    Json action() {
        const auto start = peek();
        ++index_;
        const auto name = take("identifier", "action name");
        take("lparen", "'(' after action name");
        std::string payload;
        if (peek().kind == "identifier") {
            payload = peek().literal;
            ++index_;
        }
        take("rparen", "')' after optional action payload type");
        take("arrow", "'=>' before action handler");
        const auto operation = take("identifier", "set, save or revert handler");
        std::string target;
        if (operation.literal == "set") {
            target = take("identifier", "state target after set").literal;
        } else if (operation.literal != "save" && operation.literal != "revert") {
            // Identifiers contain ASCII letters, digits and underscores only;
            // JSON and Go quoted spelling are identical for this token kind.
            fail("PL2001", span(operation), "unsupported view action handler " + Json(operation.literal).dump());
        }
        const auto end = take("semicolon", "';' after action declaration");
        return {{"Name", name.literal}, {"Payload", payload},
                {"Operation", operation.literal}, {"Target", target}, {"Span", span(start, end)}};
    }

    Json element(int depth) {
        if (depth > 8 || elements_ >= 128)
            fail("PL2001", span(peek()), "view element depth or node limit exceeded");
        ++elements_;
        const auto component = take("identifier", "component name");
        take("lparen", "'(' after component name");
        Json properties = Json::array();
        Json children = Json::array();
        std::set<std::string> names;
        while (peek().kind != "rparen") {
            const auto name = take("identifier", "property name");
            if (!names.insert(name.literal).second)
                fail("PL2001", span(name), "duplicate view property " + Json(name.literal).dump());
            take("colon", "':' after property name");
            auto propertyValue = value(true);
            auto extent = span(name);
            extent["end"] = propertyValue.at("Span").at("end");
            properties.push_back({{"Name", name.literal}, {"Value", std::move(propertyValue)},
                                  {"Span", std::move(extent)}});
            if (peek().kind != "comma")
                break;
            ++index_;
            if (peek().kind == "rparen")
                fail("PL2002", span(peek()), "expected property after ','");
        }
        auto end = take("rparen", "')' after component properties");
        if (peek().kind == "lbrace") {
            ++index_;
            while (peek().kind != "rbrace")
                children.push_back(element(depth + 1));
            end = take("rbrace", "'}' after child elements");
        } else {
            end = take("semicolon", "';' after leaf element");
        }
        return {{"Component", component.literal}, {"Properties", std::move(properties)},
                {"Children", std::move(children)}, {"Span", span(component, end)}};
    }

    Json value(bool allowReference) {
        const auto current = peek();
        Json result = {{"Kind", ""}, {"Text", ""}, {"Boolean", false},
                       {"Integer", std::int64_t{0}}, {"Span", span(current)}};
        if (current.kind == "text") {
            result["Kind"] = "text";
            result["Text"] = current.literal;
            ++index_;
        } else if (current.kind == "boolean") {
            result["Kind"] = "boolean";
            result["Boolean"] = current.literal == "true";
            ++index_;
        } else if (current.kind == "integer" || current.kind == "minus") {
            auto literal = current.literal;
            ++index_;
            if (current.kind == "minus") {
                const auto digits = take("integer", "integer after '-'");
                literal += digits.literal;
                result["Span"] = span(current, digits);
            }
            std::int64_t integer = 0;
            const auto conversion = std::from_chars(literal.data(), literal.data() + literal.size(), integer, 10);
            if (conversion.ec != std::errc{} || conversion.ptr != literal.data() + literal.size())
                fail("PL2001", result["Span"], "view integer is outside signed 64-bit range");
            result["Kind"] = "integer";
            result["Integer"] = integer;
        } else if (current.kind == "identifier") {
            if (!allowReference)
                fail("PL2001", span(current), "expected literal state initializer");
            ++index_;
            result["Kind"] = "reference";
            result["Text"] = current.literal;
            if (current.literal == "bind") {
                const auto name = take("identifier", "state name after bind");
                result["Kind"] = "binding";
                result["Text"] = name.literal;
                result["Span"] = span(current, name);
                if (peek().kind == "dot") {
                    ++index_;
                    const auto property = take("identifier", "property name after binding '.'");
                    result["Text"] = name.literal + "." + property.literal;
                    result["Span"] = span(current, property);
                }
            }
        } else {
            fail("PL2001", span(current), "expected view literal, reference or binding");
        }
        return result;
    }
};

}

Json parse(const Request& request) {
    Json result = {{"contract", "pipelang.view-parser.response.v1"},
                   {"declaration", nullptr}, {"failure", nullptr}};
    try {
        result["declaration"] = Parser(request).declaration();
    } catch (const SyntaxFailure& failure) {
        result["failure"] = failure.value;
    }
    return result;
}

}
