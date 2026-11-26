// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../../src/native/model-compiler/compiler.hpp"
#include "../../src/native/lexer/lexer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace pipelang::models;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

std::string applyEdits(std::string source, const nlohmann::json& result) {
    require(result.at("ok") == true, "formatting failed");
    const auto& edits = result.at("edits");
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
        source.replace(edit->at("offset").get<std::size_t>(), edit->at("length").get<std::size_t>(),
                       edit->at("text").get<std::string>());
    return source;
}

void sameTokens(const std::string& before, const std::string& after) {
    const auto left = pipelang::native::lex(before, pipelang::native::NumericSyntax::Binary64);
    const auto right = pipelang::native::lex(after, pipelang::native::NumericSyntax::Binary64);
    require(!left.failure && !right.failure && left.tokens.size() == right.tokens.size(),
        "formatting changed source lexical validity");
    for (std::size_t index = 0; index < left.tokens.size(); ++index)
        require(left.tokens[index].kind == right.tokens[index].kind &&
            left.tokens[index].literal == right.tokens[index].literal,
            "formatting changed a lexical token or literal");
}
}

int main() {
    try {
        const std::string source = "public Class App {\r\n"
            "public pure string Read() {\r\n"
            "// café 😀: preserve this comment and its braces { ( [\r\n"
            "var text = \"}\";\r\n"
            "   \r\n"
            "return text;\r\n"
            "}\r\n"
            "}\r\n";
        const std::string expected = "public Class App {\r\n"
            "    public pure string Read() {\r\n"
            "        // café 😀: preserve this comment and its braces { ( [\r\n"
            "        var text = \"}\";\r\n"
            "\r\n"
            "        return text;\r\n"
            "    }\r\n"
            "}\r\n";
        const auto formatted = applyEdits(source, formatSource(source, 4, true, 0, source.size()));
        require(formatted == expected, "formatting lost indentation, comments, blank lines or CRLF");
        sameTokens(source, formatted);
        require(formatSource(formatted, 4, true, 0, formatted.size()).at("edits").empty(),
            "formatting is not idempotent");

        const auto rangeStart = source.find("var text");
        const auto rangeEnd = source.find("return text");
        const auto ranged = applyEdits(source, formatSource(source, 2, true, rangeStart, rangeEnd));
        require(ranged.find("public pure string Read()") == source.find("public pure string Read()"),
            "range formatting touched a preceding line");
        require(ranged.find("\r\nreturn text;") != std::string::npos,
            "range formatting touched the line at its exclusive end");
        require(ranged.find("\r\n    var text") != std::string::npos,
            "range formatting lost enclosing source indentation");
        sameTokens(source, ranged);

        const auto tabs = applyEdits(source, formatSource(source, 4, false, 0, source.size()));
        require(tabs.find("\r\n\t\tvar text") != std::string::npos, "tab indentation lost source scope");
        sameTokens(source, tabs);
        const std::string multiline = "public Class Text {\n"
            "public pure string Read() {\n"
            "var value = \"first\n  second\n    third\";\n"
            "return value;\n"
            "}\n}\n";
        const auto multilineFormatted = applyEdits(multiline,
            formatSource(multiline, 4, true, 0, multiline.size()));
        sameTokens(multiline, multilineFormatted);
        require(multilineFormatted.find("\"first\n  second\n    third\"") != std::string::npos,
            "formatting changed multiline literal bytes");
        require(formatSource(multilineFormatted, 4, true, 0, multilineFormatted.size()).at("edits").empty(),
            "multiline literal formatting is not idempotent");

        for (unsigned size : {0U, 17U}) {
            bool rejected = false;
            try { (void)formatSource(source, size, true, 0, source.size()); }
            catch (const Diagnostic&) { rejected = true; }
            require(rejected, "invalid indentation width accepted");
        }
        bool rejected = false;
        try { (void)formatSource(source, 4, true, source.size(), 0); }
        catch (const Diagnostic&) { rejected = true; }
        require(rejected, "invalid range accepted");
        std::cout << assertions << " source formatting assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
