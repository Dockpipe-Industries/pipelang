#include "lexer.hpp"
#include "protocol.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    try {
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > pipelang::native::inputByteLimit - input.size())
                throw std::runtime_error("request byte limit");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("failed reading request");
        const auto source = pipelang::native::parseSource(input);
        const auto result = pipelang::native::lex(source);
        const auto output = pipelang::native::response(result).dump();
        if (output.size() + 1 > pipelang::native::outputByteLimit)
            throw std::runtime_error("response byte limit");
        // Buffer the complete response so protocol/resource errors leave stdout empty.
        std::cout << output << '\n';
        if (!std::cout)
            throw std::runtime_error("failed writing response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-cpp-lexer: " << error.what() << '\n';
        return 2;
    }
}
