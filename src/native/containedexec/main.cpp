// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "containment.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace pipelang::contained;
    try {
        require(argc >= 3, "usage: pipelang-contained job|unit --output PATH [--estate PATH] [--timeout SECONDS] -- COMMAND");
        int separator = 2;
        while (separator < argc && std::string(argv[separator]) != "--")
            ++separator;
        require(separator + 1 < argc, "contained execution requires a command after --");
        const std::string mode = argv[1];
        const bool inside = mode == "--inside-job" || mode == "--inside-unit";
        require(inside || mode == "job" || mode == "unit", "unknown containment mode");
        Request request;
        request.job = mode == "job" || mode == "--inside-job";
        Arguments arguments(separator, argv, 2);
        if (!inside || !request.job) {
            request.output = arguments.take("--output");
            request.estate = arguments.optional("--estate", request.output.parent_path().string());
            const auto timeout = arguments.optional("--timeout", "900");
            std::size_t used = 0;
            const auto seconds = std::stoll(timeout, &used);
            require(used == timeout.size() && seconds > 0 && seconds <= (request.job ? 21600 : 1800), "invalid containment deadline");
            request.timeout = std::chrono::seconds(seconds);
            const auto estateLimit = arguments.optional("--estate-limit-mib", "8192");
            used = 0;
            const auto mebibytes = std::stoull(estateLimit, &used);
            require(used == estateLimit.size() && mebibytes > 0 && mebibytes <= 8192, "estate limit must be 1..8192 MiB");
            request.estateLimit = mebibytes * mebibyte;
        }
        arguments.finish();
        for (int index = separator + 1; index < argc; ++index)
            request.command.emplace_back(argv[index]);
        if (inside)
            return replace(request);
        return execute(request, fs::canonical(argv[0]));
    } catch (const std::exception& error) {
        std::cerr << "pipelang-contained: " << error.what() << '\n';
        return 2;
    }
}
