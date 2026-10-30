#include "parser.hpp"

#include <initializer_list>
#include <set>
#include <stdexcept>

namespace pipelang::view {
namespace {

void exactMembers(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size())
        throw std::runtime_error("invalid request members");
    for (const auto name : names) {
        if (!object.contains(name))
            throw std::runtime_error("missing request member");
    }
}

std::string stringMember(const Json& object, const char* name) {
    const auto& value = object.at(name);
    if (!value.is_string())
        throw std::runtime_error("request member must be a string");
    auto text = value.get<std::string>();
    if (text.size() > 65536)
        throw std::runtime_error("request string limit");
    return text;
}

int offsetMember(const Json& object, const char* name) {
    const auto& value = object.at(name);
    if (!value.is_number_integer() || value < 0 || value > 65536)
        throw std::runtime_error("request offset must be an integer within 64 KiB");
    return value.get<int>();
}

}

Request decodeRequest(const std::string& input) {
    if (input.size() > messageByteLimit)
        throw std::runtime_error("request byte limit");
    std::size_t nodes = 0;
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        using Event = Json::parse_event_t;
        if (depth > 4)
            throw std::runtime_error("request nesting limit");
        if (event == Event::object_start || event == Event::array_start || event == Event::value) {
            ++nodes;
            if (nodes > 400000)
                throw std::runtime_error("request node limit");
        }
        if (event == Event::object_start)
            objectKeys.emplace_back();
        if (event == Event::key) {
            const auto key = value.get<std::string>();
            if (objectKeys.empty() || !objectKeys.back().insert(key).second)
                throw std::runtime_error("duplicate request member");
        }
        if (event == Event::object_end)
            objectKeys.pop_back();
        return true;
    };
    const auto inputObject = Json::parse(input, callback);
    exactMembers(inputObject, {"contract", "file", "source_bytes", "tokens"});
    if (stringMember(inputObject, "contract") != "pipelang.view-parser.request.v1")
        throw std::runtime_error("unsupported request contract");
    Request request{stringMember(inputObject, "file"), offsetMember(inputObject, "source_bytes"), {}};
    const auto& tokens = inputObject.at("tokens");
    if (request.file.empty() || request.sourceBytes == 0 || !tokens.is_array() || tokens.empty() ||
        tokens.size() > static_cast<std::size_t>(request.sourceBytes + 1))
        throw std::runtime_error("invalid request source or token count");
    static const std::set<std::string> kinds = {
        "eof", "identifier", "integer", "float", "text", "boolean", "interface",
        "class", "struct", "public", "private", "lbrace", "rbrace", "lparen",
        "rparen", "lbracket", "rbracket", "colon", "semicolon", "comma", "dot",
        "question", "assign", "arrow", "plus", "minus", "star", "slash", "bang",
        "equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "and", "or"
    };
    int previousEnd = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& item = tokens.at(index);
        exactMembers(item, {"kind", "literal", "start", "end"});
        Token token{stringMember(item, "kind"), stringMember(item, "literal"),
                    offsetMember(item, "start"), offsetMember(item, "end")};
        if (kinds.count(token.kind) == 0 || token.start < previousEnd || token.end < token.start || token.end > request.sourceBytes)
            throw std::runtime_error("invalid request token kind or extent");
        if (index + 1 == tokens.size()) {
            if (token.kind != "eof" || token.start != request.sourceBytes || token.end != request.sourceBytes || !token.literal.empty())
                throw std::runtime_error("missing canonical EOF");
        } else if (token.kind == "eof" || token.start == token.end) {
            throw std::runtime_error("early EOF or empty token");
        }
        previousEnd = token.end;
        request.tokens.push_back(std::move(token));
    }
    return request;
}

}
