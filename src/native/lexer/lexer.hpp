#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pipelang::native {

struct Token {
    std::string kind;
    std::string literal;
    std::size_t start;
    std::size_t end;
};

struct LexicalFailure {
    std::string code;
    std::string message;
    std::size_t start;
    std::size_t end;
};

struct LexResult {
    std::vector<Token> tokens;
    std::optional<LexicalFailure> failure;
};

enum class NumericSyntax { Legacy, Binary64 };

LexResult lex(const std::string& source);
LexResult lex(const std::string& source, NumericSyntax numericSyntax);

}
