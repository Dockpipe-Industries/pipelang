// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"

#include <array>
#include <iostream>
#include <set>

int main() {
    using nlohmann::json;
    try {
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > pipelang::models::messageLimit - input.size())
                throw std::runtime_error("request byte limit exceeded");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("failed to read request");
        std::vector<std::set<std::string>> objectKeys;
        auto request = json::parse(input, [&](int depth, json::parse_event_t event, json& parsed) {
            if (depth > 16)
                throw std::runtime_error("request JSON nesting limit exceeded");
            if (event == json::parse_event_t::object_start)
                objectKeys.emplace_back();
            else if (event == json::parse_event_t::object_end)
                objectKeys.pop_back();
            else if (event == json::parse_event_t::key && !objectKeys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("duplicate request key");
            return true;
        });
        if (!request.is_object() || !request.contains("protocol") || !request.contains("source") ||
            !request["protocol"].is_string() || request["protocol"] != "pipelang.native-model.v1" ||
            !request["source"].is_string())
            throw std::runtime_error("invalid native model request");
        for (auto item = request.begin(); item != request.end(); ++item) {
            if (item.key() != "protocol" && item.key() != "source" && item.key() != "namespace")
                throw std::runtime_error("unknown request field");
        }
        if (request.contains("namespace") && !request["namespace"].is_string())
            throw std::runtime_error("namespace must be a string");
        json response;
        try {
            response = pipelang::models::compile(request["source"].get<std::string>(),
                                                 request.value("namespace", "pipelang_models"));
        } catch (const pipelang::models::Diagnostic& diagnostic) {
            response = {{"protocol", "pipelang.native-model.v1"}, {"ok", false},
                        {"diagnostics", json::array({{{"code", "PLM1001"}, {"message", diagnostic.what()},
                                                     {"offset", diagnostic.offset}}})}};
        }
        const auto output = response.dump();
        if (output.size() + 1 > pipelang::models::messageLimit)
            throw std::runtime_error("response byte limit exceeded");
        std::cout << output << '\n';
        if (!std::cout)
            throw std::runtime_error("failed to write response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-modelc: " << error.what() << '\n';
        return 2;
    }
}
