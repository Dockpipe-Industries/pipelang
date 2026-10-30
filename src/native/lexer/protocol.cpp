#include "protocol.hpp"

#include <set>
#include <stdexcept>
#include <vector>

namespace pipelang::native {

std::string parseSource(const std::string& input) {
    std::size_t nodes = 0;
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                              nlohmann::json& value) {
        using Event = nlohmann::json::parse_event_t;
        if (depth > 16)
            throw std::runtime_error("request nesting limit");
        if (event == Event::object_start || event == Event::array_start || event == Event::value) {
            ++nodes;
            if (nodes > 64)
                throw std::runtime_error("request node limit");
        }
        if (event == Event::object_start)
            objectKeys.emplace_back();
        if (event == Event::key) {
            const auto key = value.get<std::string>();
            if (objectKeys.empty() || !objectKeys.back().insert(key).second)
                throw std::runtime_error("duplicate request field");
        }
        if (event == Event::object_end)
            objectKeys.pop_back();
        return true;
    };
    const auto request = nlohmann::json::parse(input, callback);
    if (!request.is_object() || request.size() != 2 ||
        !request.contains("contract") || !request.contains("source"))
        throw std::runtime_error("invalid request fields");
    if (!request.at("contract").is_string() ||
        request.at("contract").get<std::string>() != "pipelang.lexer.request.v1")
        throw std::runtime_error("invalid request contract");
    if (!request.at("source").is_string())
        throw std::runtime_error("request source must be a string");
    auto source = request.at("source").get<std::string>();
    if (source.size() > 65536)
        throw std::runtime_error("source byte limit");
    return source;
}

nlohmann::json response(const LexResult& result) {
    nlohmann::json output = {
        {"contract", "pipelang.lexer.response.v1"},
        {"tokens", nullptr},
        {"failure", nullptr}
    };
    if (result.failure) {
        const auto& failure = *result.failure;
        output["failure"] = {
            {"code", failure.code},
            {"message", failure.message},
            {"start", failure.start},
            {"end", failure.end}
        };
        return output;
    }
    output["tokens"] = nlohmann::json::array();
    for (const auto& token : result.tokens) {
        output["tokens"].push_back({
            {"kind", token.kind},
            {"literal", token.literal},
            {"start", token.start},
            {"end", token.end}
        });
    }
    return output;
}

}
