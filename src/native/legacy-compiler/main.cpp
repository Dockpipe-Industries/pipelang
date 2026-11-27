// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"

#include <iostream>
#include <iterator>
#include <string_view>

#ifdef PIPELANG_NATIVE_BUILD_COMMAND
namespace {

int buildNativeExecutable(int argc, char** argv)
{
    using namespace pipelang::tools;
    Arguments arguments(argc, argv, 2);
    const auto backend = arguments.optional("--backend", "native");
    require(backend == "native", "unsupported executable build backend '" + backend +
        "'; use pipelang-modelc for C++ model generation");
    const fs::path installPath(PIPELANG_NATIVE_OBJECT_COMPILER_RELATIVE_PATH);
    require(!installPath.empty(), "this PipeLang SDK does not include the native executable compiler");

    const auto compiler = (executablePath().parent_path() / installPath).lexically_normal();
    regularFile(compiler);
    std::vector<std::string> invocation{compiler.string(), "--executable"};
    auto remaining = arguments.rest();
    invocation.insert(invocation.end(), std::make_move_iterator(remaining.begin()),
                      std::make_move_iterator(remaining.end()));
    const auto result = process(invocation, {}, std::chrono::seconds(120));
    std::cout << result.output;
    std::cerr << result.diagnostic;
    return result.exit;
}

}
#endif

int main(int argc, char** argv) {
    try {
#ifdef PIPELANG_NATIVE_BUILD_COMMAND
        if (argc >= 2 && std::string_view(argv[1]) == "build")
            return buildNativeExecutable(argc, argv);
#endif
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "development unknown\n";
            return 0;
        }
        const auto status = pipelang::legacy::command({argv + 1, argv + argc}, std::cin, std::cout);
#ifdef PIPELANG_NATIVE_BUILD_COMMAND
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help"))
            std::cout << "pipelang build [--backend native] --source FILE --entry METHOD --output FILE [options]\n"
                         "  Build a machine-code executable. Use pipelang-modelc for C++ model generation.\n";
#endif
        return status;
    } catch (const pipelang::legacy::Diagnostic& error) {
        std::cerr << error.file << ':' << error.start << ": error[" << error.code << "]: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-native: " << error.what() << '\n';
        return 2;
    }
}
