#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pipelang::view {

using Json = nlohmann::json;

struct Token {
    std::string kind;
    std::string literal;
    int start;
    int end;
};

struct Request {
    std::string file;
    int sourceBytes;
    std::vector<Token> tokens;
};

inline constexpr std::size_t messageByteLimit = 16U << 20;

Request decodeRequest(const std::string& input);
Json parse(const Request& request);

}
