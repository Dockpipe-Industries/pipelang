// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"

namespace pipelang::model_view {
Json styleDescriptor(const View &view)
{
    Json descriptor = {{"protocol", styleDescriptorProtocol}, {"view", view.name},
        {"style", nullptr}, {"scope", "resolved-authored"}, {"properties", Json::array()}};
    if (!view.style)
        return descriptor;

    descriptor["style"] = view.style->name;
    for (const auto &[name, value] : view.style->properties) {
        if (value.kind == styles::ValueKind::Length) {
            descriptor["properties"].push_back({{"name", name}, {"type", "length"},
                {"unit", "px"}, {"value", value.length}});
        } else {
            auto color = value.color;
            for (auto &digit : color) {
                if (digit >= 'a' && digit <= 'f')
                    digit = static_cast<char>(digit - 'a' + 'A');
            }
            descriptor["properties"].push_back({{"name", name}, {"type", "color"},
                {"value", color}});
        }
    }
    return descriptor;
}
}
