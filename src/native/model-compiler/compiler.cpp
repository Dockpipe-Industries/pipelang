// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "syntax.hpp"

#include <cctype>

namespace pipelang::models {

nlohmann::json compile(const std::string& source, const std::string& nameSpace) {
    if (nameSpace.empty() || nameSpace.size() > 96 || nameSpace.starts_with('_'))
        throw Diagnostic("invalid generated namespace", 0);
    for (std::size_t index = 0; index < nameSpace.size(); ++index) {
        const auto character = static_cast<unsigned char>(nameSpace[index]);
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              character == '_' || (index > 0 && character >= '0' && character <= '9')))
            throw Diagnostic("invalid generated namespace", 0);
    }
    // Reuse the identifier admission, including native keyword reservations.
    parse("public Record " + nameSpace + " { }");
    auto program = parse(source);
    check(program);
    return {{"protocol", "pipelang.native-model.v1"}, {"ok", true},
            {"header", generate(program, nameSpace)}, {"manifest", manifest(program, nameSpace)}};
}

}
