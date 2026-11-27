// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "BindingModel.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "pipelang.qt-model-bindings.v1\n";
            return 0;
        }
        std::map<std::string, std::string> options;
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::runtime_error("duplicate or missing Qt binding option");
        }
        if (options.size() != 4 || !options.contains("--manifest") || !options.contains("--model") ||
            !options.contains("--namespace") || !options.contains("--adapter"))
            throw std::runtime_error("expected --manifest --model --namespace --adapter");
        std::ifstream input(options.at("--manifest"), std::ios::binary);
        if (!input)
            throw std::runtime_error("model manifest read failed");
        std::string bytes;
        char buffer[8192];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count > 4 * 1024 * 1024 - bytes.size())
                throw std::runtime_error("model manifest exceeds Qt profile");
            bytes.append(buffer, count);
        }
        if (!input.eof())
            throw std::runtime_error("model manifest read failed");
        std::vector<std::set<std::string>> objects;
        const auto manifest = nlohmann::json::parse(bytes,
            [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
                if (depth > 64)
                    throw std::runtime_error("model manifest nesting exceeds Qt profile");
                if (event == nlohmann::json::parse_event_t::object_start) {
                    objects.emplace_back();
                } else if (event == nlohmann::json::parse_event_t::object_end) {
                    objects.pop_back();
                } else if (event == nlohmann::json::parse_event_t::key) {
                    if (!objects.back().insert(value.get<std::string>()).second)
                        throw std::runtime_error("duplicate model manifest key");
                }
                return true;
            });
        const auto model = pipelang::qt_bindings::admit(manifest, options.at("--model"));
        const auto header = pipelang::qt_bindings::emit(model, options.at("--namespace"), options.at("--adapter"));
        if (header.size() > 4 * 1024 * 1024)
            throw std::runtime_error("generated Qt binding exceeds profile");
        std::cout << header;
        return std::cout ? 0 : 2;
    } catch (const nlohmann::json::exception&) {
        std::cerr << "Qt binding refused: malformed model manifest\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Qt binding refused: " << error.what() << '\n';
        return 2;
    }
}
