#include "parser.hpp"

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
            if (count > pipelang::view::messageByteLimit - input.size())
                throw std::runtime_error("request byte limit");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("failed reading request");
        const auto request = pipelang::view::decodeRequest(input);
        const auto output = pipelang::view::parse(request).dump();
        if (output.size() + 1 > pipelang::view::messageByteLimit)
            throw std::runtime_error("response byte limit");
        // Complete encoding precedes stdout: protocol failures cannot expose a
        // partially admitted declaration or be mistaken for a syntax failure.
        std::cout << output << '\n';
        if (!std::cout)
            throw std::runtime_error("failed writing response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-cpp-view-parser: " << error.what() << '\n';
        return 2;
    }
}
