// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../lexer/lexer.hpp"
#include <map>
#include <optional>
#include <string>

namespace pipelang::model_view::styles {
enum class ValueKind { Length, Color };
struct Value {
    ValueKind kind = ValueKind::Length;
    double length = 0;
    std::string color;
};
struct ResolvedStyle {
    std::string name;
    std::map<std::string, Value> properties;
};
struct Failure {
    std::string message;
    std::size_t start;
    std::size_t end;
};
using StyleSet = std::map<std::string, ResolvedStyle>;
// Consume and validate every leading declaration, including unselected styles.
StyleSet parse(const std::vector<native::Token> &tokens, std::size_t &index);
// Consume an optional `style Name` after the view model parameter list.
std::optional<ResolvedStyle> selection(const StyleSet &styles,
                                     const std::vector<native::Token> &tokens,
                                     std::size_t &index);
}
