#pragma once

#include "lexer.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace pipelang::native {

inline constexpr std::size_t inputByteLimit = 512 * 1024;
inline constexpr std::size_t outputByteLimit = 16 * 1024 * 1024;

std::string parseSource(const std::string& input);
nlohmann::json response(const LexResult& result);

}
