// SPDX-License-Identifier: Apache-2.0
#include "Tests.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "select a fresh fixture directory");
        TestContext context(argv[1]);
        Functional(context);
        Adversarial(context);
        Backup(context);
        std::cout << "SQLite typed/FTS5/lifetime/cancellation/backup fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
