// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"
#include "OutputBudgetStream.hpp"
#include "../lexer/lexer.hpp"

#include <algorithm>
#include <ostream>

namespace pipelang::models {

nlohmann::json formatSource(const std::string& source, unsigned tabSize, bool insertSpaces,
                            std::size_t rangeStart, std::size_t rangeEnd) {
    if (source.size() > resourceBudget().inputBytes)
        throw Diagnostic("format input_bytes resource budget exceeded; configure input_bytes", 0);
    if (tabSize < 1 || tabSize > 16 || rangeStart > rangeEnd || rangeEnd > source.size())
        throw Diagnostic("format requires tab_size 1..16 and a valid source byte range", 0);

    const auto lexical = native::lex(source, native::NumericSyntax::Binary64, resourceBudget().tokens);
    if (lexical.failure)
        throw Diagnostic(lexical.failure->message, lexical.failure->start);

    auto edits = nlohmann::json::array();
    std::size_t tokenIndex = 0;
    std::size_t braces = 0;
    std::size_t parentheses = 0;
    std::size_t brackets = 0;
    std::size_t outputBytes = 0;
    std::size_t literalEnd = 0;
    for (std::size_t start = 0; start < source.size();) {
        const auto newline = source.find('\n', start);
        const auto end = newline == std::string::npos ? source.size() : newline;
        auto content = start;
        while (content < end && (source[content] == ' ' || source[content] == '\t'))
            ++content;

        const bool empty = content == end || (content + 1 == end && source[content] == '\r');
        auto indentation = braces;
        if (!empty && source[content] == '}' && indentation != 0)
            --indentation;
        if (!empty && (parentheses != 0 || brackets != 0) &&
            source[content] != ')' && source[content] != ']')
            ++indentation;

        const auto desired = empty ? std::string{} : std::string(
            indentation * (insertSpaces ? tabSize : 1U), insertSpaces ? ' ' : '\t');
        const bool selected = rangeStart == rangeEnd
            ? start <= rangeStart && end >= rangeStart
            : start < rangeEnd && end >= rangeStart;
        const bool insideLiteral = start < literalEnd;
        if (selected && !insideLiteral && source.compare(start, content - start, desired) != 0) {
            nlohmann::json edit{{"offset", start}, {"length", content - start}, {"text", desired}};
            const auto bytes = edit.dump().size() + 1;
            if (budgetExceeded(outputBytes, bytes, resourceBudget().outputBytes))
                throw Diagnostic("format output_bytes resource budget exceeded; configure output_bytes", start);
            outputBytes += bytes;
            edits.push_back(std::move(edit));
        }

        while (tokenIndex < lexical.tokens.size() && lexical.tokens[tokenIndex].start < end) {
            const auto& token = lexical.tokens[tokenIndex++];
            if (token.kind == "text") {
                literalEnd = token.end;
                continue;
            }

            if (token.literal == "{")
                ++braces;
            else if (token.literal == "}" && braces != 0)
                --braces;
            else if (token.literal == "(")
                ++parentheses;
            else if (token.literal == ")" && parentheses != 0)
                --parentheses;
            else if (token.literal == "[")
                ++brackets;
            else if (token.literal == "]" && brackets != 0)
                --brackets;
        }
        if (newline == std::string::npos)
            break;
        start = newline + 1;
    }

    nlohmann::json response{{"protocol", "pipelang.native-model-format.v1"}, {"ok", true},
        {"edits", std::move(edits)}, {"diagnostics", nlohmann::json::array()}};
    OutputBudgetStream counter(resourceBudget().outputBytes - 1);
    std::ostream output(&counter);
    output << response;
    if (!output || counter.exceeded())
        throw Diagnostic("format output_bytes resource budget exceeded; configure output_bytes", 0);
    return response;
}

} // namespace pipelang::models
