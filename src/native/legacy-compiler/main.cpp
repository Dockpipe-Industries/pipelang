// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "development unknown\n";
            return 0;
        }
        return pipelang::legacy::command({argv + 1, argv + argc}, std::cin, std::cout);
    } catch (const pipelang::legacy::Diagnostic& error) {
        std::cerr << error.file << ':' << error.start << ": error[" << error.code << "]: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-native: " << error.what() << '\n';
        return 2;
    }
}
