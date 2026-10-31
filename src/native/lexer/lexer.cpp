#include "lexer.hpp"

#include <string_view>
#include <utility>

namespace pipelang::native {
namespace {

bool isSpace(unsigned char character) {
    // Match unicode.IsSpace(rune(byte)), without decoding source UTF-8.
    return (character >= 9 && character <= 13) || character == 32 ||
           character == 0x85 || character == 0xa0;
}

bool isDigit(unsigned char character) {
    return character >= '0' && character <= '9';
}

bool isIdentifierStart(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_';
}

std::string identifierKind(std::string_view literal) {
    if (literal == "Interface")
        return "interface";
    if (literal == "Class")
        return "class";
    if (literal == "Struct")
        return "struct";
    if (literal == "public" || literal == "private")
        return std::string(literal);
    if (literal == "true" || literal == "false")
        return "boolean";
    return "identifier";
}

std::string quotedByte(unsigned char character) {
    switch (character) {
    case '\a':
        return "'\\a'";
    case '\b':
        return "'\\b'";
    case '\f':
        return "'\\f'";
    case '\n':
        return "'\\n'";
    case '\r':
        return "'\\r'";
    case '\t':
        return "'\\t'";
    case '\v':
        return "'\\v'";
    case '\\':
        return "'\\\\'";
    case '\'':
        return "'\\\''";
    default:
        break;
    }

    constexpr char hexadecimal[] = "0123456789abcdef";
    if (character < 32 || character == 0x7f) {
        std::string quoted = "'\\x00'";
        quoted[3] = hexadecimal[character >> 4];
        quoted[4] = hexadecimal[character & 15];
        return quoted;
    }
    if ((character >= 0x80 && character <= 0xa0) || character == 0xad) {
        std::string quoted = "'\\u0000'";
        quoted[5] = hexadecimal[character >> 4];
        quoted[6] = hexadecimal[character & 15];
        return quoted;
    }

    std::string quoted = "'";
    if (character < 0x80) {
        quoted += static_cast<char>(character);
    } else {
        quoted += static_cast<char>(0xc0 | (character >> 6));
        quoted += static_cast<char>(0x80 | (character & 0x3f));
    }
    quoted += '\'';
    return quoted;
}

std::optional<char> decodeEscape(char character) {
    // Go unquotes exactly two source bytes, so numeric/Unicode escapes fail.
    switch (character) {
    case 'a':
        return '\a';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'v':
        return '\v';
    case '\\':
        return '\\';
    case '"':
        return '"';
    default:
        return std::nullopt;
    }
}

LexResult failure(std::string code, std::string message,
                  std::size_t start, std::size_t end) {
    return {{}, LexicalFailure{std::move(code), std::move(message), start, end}};
}

}

LexResult lex(const std::string& source) {
    return lex(source, NumericSyntax::Legacy);
}

LexResult lex(const std::string& source, NumericSyntax numericSyntax) {
    LexResult result;
    result.tokens.reserve(128);
    std::size_t offset = 0;
    const auto append = [&](std::string kind, std::size_t start, std::size_t end) {
        result.tokens.push_back({std::move(kind), source.substr(start, end - start), start, end});
    };

    while (offset < source.size()) {
        const auto character = static_cast<unsigned char>(source[offset]);
        if (isSpace(character)) {
            ++offset;
            continue;
        }
        if (character == '/' && offset + 1 < source.size() && source[offset + 1] == '/') {
            offset += 2;
            while (offset < source.size() && source[offset] != '\n')
                ++offset;
            continue;
        }
        if (isIdentifierStart(character)) {
            const auto start = offset++;
            while (offset < source.size() &&
                   (isIdentifierStart(source[offset]) || isDigit(source[offset])))
                ++offset;
            append(identifierKind(std::string_view(source).substr(start, offset - start)), start, offset);
            continue;
        }
        if (isDigit(character)) {
            const auto start = offset++;
            while (offset < source.size() && isDigit(source[offset]))
                ++offset;
            bool floatingPoint = false;
            if (offset < source.size() && source[offset] == '.') {
                floatingPoint = true;
                ++offset;
                if (offset == source.size() || !isDigit(source[offset]))
                    return failure("PL1002", "invalid float literal", start, offset);
                while (offset < source.size() && isDigit(source[offset]))
                    ++offset;
            }
            if (numericSyntax == NumericSyntax::Binary64 && offset < source.size() &&
                (source[offset] == 'e' || source[offset] == 'E')) {
                floatingPoint = true;
                ++offset;
                if (offset < source.size() && (source[offset] == '+' || source[offset] == '-'))
                    ++offset;
                if (offset == source.size() || !isDigit(source[offset]))
                    return failure("PL1002", "invalid float exponent", start, offset);
                while (offset < source.size() && isDigit(source[offset]))
                    ++offset;
            }
            if (numericSyntax == NumericSyntax::Binary64 && offset < source.size() && source[offset] == 'f') {
                ++offset;
                append("float32", start, offset);
            } else {
                append(floatingPoint ? "float" : "integer", start, offset);
            }
            continue;
        }
        if (character == '"') {
            const auto start = offset++;
            std::string decoded;
            bool terminated = false;
            while (offset < source.size()) {
                if (source[offset] == '"') {
                    ++offset;
                    terminated = true;
                    break;
                }
                if (source[offset] == '\\') {
                    if (offset + 1 == source.size())
                        return failure("PL1004", "unterminated escape sequence", start, source.size());
                    const auto escaped = decodeEscape(source[offset + 1]);
                    if (!escaped)
                        return failure("PL1003", "invalid escape sequence", offset, offset + 2);
                    decoded += *escaped;
                    offset += 2;
                    continue;
                }
                decoded += source[offset++];
            }
            if (!terminated)
                return failure("PL1004", "unterminated string literal", start, source.size());
            result.tokens.push_back({"text", std::move(decoded), start, offset});
            continue;
        }

        const auto start = offset;
        std::string kind;
        switch (character) {
        case '{':
            kind = "lbrace";
            break;
        case '}':
            kind = "rbrace";
            break;
        case '(':
            kind = "lparen";
            break;
        case ')':
            kind = "rparen";
            break;
        case '[':
            kind = "lbracket";
            break;
        case ']':
            kind = "rbracket";
            break;
        case ':':
            kind = "colon";
            break;
        case ';':
            kind = "semicolon";
            break;
        case ',':
            kind = "comma";
            break;
        case '.':
            kind = "dot";
            break;
        case '?':
            kind = "question";
            break;
        case '+':
            kind = "plus";
            break;
        case '-':
            kind = "minus";
            break;
        case '*':
            kind = "star";
            break;
        case '/':
            kind = "slash";
            break;
        case '=':
            kind = "assign";
            break;
        case '!':
            kind = "bang";
            break;
        case '<':
            kind = "less";
            break;
        case '>':
            kind = "greater";
            break;
        default:
            break;
        }
        if (offset + 1 < source.size()) {
            const auto pair = std::string_view(source).substr(offset, 2);
            if (pair == "=>")
                kind = "arrow";
            else if (pair == "==")
                kind = "equal";
            else if (pair == "!=")
                kind = "not_equal";
            else if (pair == "<=")
                kind = "less_equal";
            else if (pair == ">=")
                kind = "greater_equal";
            else if (pair == "&&")
                kind = "and";
            else if (pair == "||")
                kind = "or";
            if (pair == "=>" || pair == "==" || pair == "!=" || pair == "<=" ||
                pair == ">=" || pair == "&&" || pair == "||")
                ++offset;
        }
        if (kind.empty())
            return failure("PL1001", "unexpected character " + quotedByte(character), start, start + 1);
        ++offset;
        append(std::move(kind), start, offset);
    }
    result.tokens.push_back({"eof", "", source.size(), source.size()});
    return result;
}

}
