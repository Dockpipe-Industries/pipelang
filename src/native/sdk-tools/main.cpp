// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
#include "package.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc >= 2, "usage: pipelang-sdk-tool manifest|verify|bindings [arguments]");
        const std::string command = argv[1];
        Arguments arguments(argc, argv, 2);
        if (command == "sdk-package") {
            const fs::path prefix = arguments.take("--prefix");
            const fs::path manifest = arguments.take("--manifest");
            const auto expected = arguments.take("--sha256");
            const fs::path output = arguments.take("--output");
            arguments.finish();
            packageSdk(prefix, manifest, expected, output);
        } else if (command == "sdk-extract") {
            const fs::path manifest = arguments.take("--manifest");
            const auto expected = arguments.take("--sha256");
            const fs::path output = arguments.take("--output");
            arguments.finish();
            extractSdk(manifest, expected, output);
        } else if (command == "audit-exec") {
            const fs::path trace = arguments.take("--trace");
            const fs::path output = arguments.take("--output");
            arguments.finish();
            atomicWrite(output, canonical(auditExecutions(trace)));
        } else if (command == "inventory") {
            const fs::path root = arguments.take("--source-root");
            const fs::path output = arguments.take("--output");
            arguments.finish();
            atomicWrite(output, canonical(dependencyInventory(root)));
        } else if (command == "manifest" || command == "verify") {
            const fs::path prefix = fs::absolute(arguments.take("--prefix")).lexically_normal();
            const fs::path manifest = fs::absolute(arguments.take("--manifest")).lexically_normal();
            if (command == "verify") {
                const auto expected = arguments.take("--sha256");
                arguments.finish();
                verifyManifest(prefix, manifest, expected);
            } else {
                const auto roots = arguments.rest();
                require(!roots.empty() && roots.front() == "--roots", "manifest requires --roots");
                Json selected = Json::array();
                for (std::size_t index = 1; index < roots.size(); ++index)
                    selected.push_back(roots[index]);
                recordManifest(prefix, manifest, selected);
            }
        } else if (command == "bindings") {
            const fs::path prefix = arguments.take("--sdk-prefix");
            const fs::path manifest = arguments.take("--sdk-manifest");
            const auto expected = arguments.take("--sdk-manifest-sha256");
            const fs::path modelTool = arguments.take("--model-compiler");
            const fs::path viewTool = arguments.take("--view-compiler");
            const fs::path model = arguments.take("--model");
            const fs::path view = arguments.take("--view");
            const auto nameSpace = arguments.take("--namespace");
            const fs::path output = arguments.take("--output-directory");
            const bool inspection = arguments.flag("--style-inspection");
            arguments.finish();
            generateBindings(prefix, manifest, expected, modelTool, viewTool, model, view, nameSpace, output, inspection);
        } else {
            throw std::runtime_error("unknown SDK tool command");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-sdk-tool: " << error.what() << '\n';
        return 2;
    }
}
