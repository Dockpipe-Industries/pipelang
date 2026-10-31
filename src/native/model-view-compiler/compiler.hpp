// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include "styles.hpp"
#include <string>
#include <vector>

namespace pipelang::model_view {
using Json = nlohmann::json;
inline constexpr std::size_t messageByteLimit = 1U << 20;
inline constexpr const char *styleDescriptorProtocol = "pipelang.native-style-descriptor.v1";
struct Binding {
    std::string expression;
    std::string type;
    std::string identity;
};
struct Action {
    std::string method;
    std::string payloadType;
    bool asynchronous = false;
    bool cancellation = false;
    std::vector<bool> cancellationParameters;
};
struct Node {
    std::string kind;
    std::string identity;
    std::string label;
    std::string enabledExpression = "true";
    Binding value;
    Action action;
    bool sensitive = false;
    int width = 0;
    int height = 0;
    std::vector<Node> children;
};
struct View {
    std::string name;
    std::string model;
    std::string nameSpace;
    std::string modelHeader;
    Node root;
    std::optional<styles::ResolvedStyle> style;
};
Json styleDescriptor(const View &view);
Json compile(const Json &request);
std::string emit(const View &view);
Json decode(const std::string &input);
}
