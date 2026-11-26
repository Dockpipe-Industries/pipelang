// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace json_contracts {

inline std::array<std::string, 41> fixtureInputs()
{
    const auto repeat = [](const std::string& text, std::size_t count) {
        std::string result;
        result.reserve(text.size() * count);
        for (std::size_t index = 0; index < count; ++index)
            result += text;
        return result;
    };
    return {
        R"({"nil":null,"flag":true,"number":7,"unknown":[]})",
        R"({"a":1,"a":2})", R"({"a":1,"\u0061":2})",
        R"([{"nested":{"x":0,"x":1}}])", R"([{"a":1},{"a":2}])",
        std::string{'"', static_cast<char>(0xff), '"'}, R"("\ud800")",
        "[1,", "null false", "NaN", "1e400", "9223372036854775807",
        "-9223372036854775808", "9223372036854775808", "1.25", "[]",
        "[" + repeat("7,", 8192) + "7]", "\"" + std::string(1048577, 'x') + "\"",
        std::string(4194300, ' ') + "null", "null", "[1,2]", "[1,2]",
        "[[]]", "[[[]]]", R"({"abc":0})", R"({"abcd":0})", R"("abc")", R"("abcd")",
        std::string("\xef\xbb\xbf") + "null", R"("\u0000")", std::string{'"', '\n', '"'},
        std::string{'\0', 'n', '\0', 'u', '\0', 'l', '\0', 'l'},
        "[" + repeat("0,", 1000) + "0]", "null", "true", "{}",
        R"({"\u00e9":0,"é":1})", R"("\ud83d\ude00")",
        std::string{'"', static_cast<char>(0xc0), static_cast<char>(0xaf), '"'},
        "-9223372036854775809", std::string("null\0true", 9)
    };
}

}
