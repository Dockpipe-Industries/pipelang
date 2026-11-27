// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "compiler.hpp"
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

int main()
{
    using namespace pipelang::model_view;
    Json response;
    try {
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > messageByteLimit - input.size())
                throw std::runtime_error("request byte limit exceeded");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("request read failed");
        response = compile(decode(input));
    } catch (const std::exception &error) {
        response = {{"protocol", "pipelang.native-model-view.v1"}, {"ok", false},
            {"diagnostics", Json::array({{{"code", "PMV0001"}, {"message", error.what()},
                {"start", 0}, {"end", 0}}})}};
    }
    try {
        const auto output = response.dump();
        if (output.size() + 1 > messageByteLimit)
            throw std::runtime_error("response byte limit exceeded");
        std::cout << output << '\n';
        return std::cout ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << "pipelang-native-model-view-compiler: " << error.what() << '\n';
        return 2;
    }
}
