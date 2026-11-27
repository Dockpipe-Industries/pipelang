// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "bindings.hpp"
#include "package.hpp"
#include "source_set.hpp"
#include "SourceProject.hpp"
#include "../model-compiler/ScopedResourceBudget.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc >= 2, "usage: pipelang-sdk-tool manifest|verify|bindings|model-bindings|source-set-info [arguments]");
        const std::string command = argv[1];
        Arguments arguments(argc, argv, 2);
        const fs::path budgetPath = arguments.optional("--resource-budget");
        pipelang::models::ResourceBudget selectedBudget;
        std::string budgetHash;
        if (!budgetPath.empty()) {
            require(budgetPath.is_absolute(), "resource budget profile path must be absolute");
            const auto bytes = readFile(budgetPath, 65536);
            selectedBudget = pipelang::models::ResourceBudget::fromText(bytes);
            budgetHash = sha256(bytes);
        }
        const pipelang::models::ScopedResourceBudget budgetScope(selectedBudget, budgetPath.generic_string(), budgetHash);
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
        } else if (command == "project-compile-info") {
            const fs::path prefix = arguments.take("--sdk-prefix");
            const fs::path manifest = arguments.take("--sdk-manifest");
            const auto expected = arguments.take("--sdk-manifest-sha256");
            const fs::path modelTool = arguments.take("--model-compiler");
            const fs::path projectRoot = arguments.take("--project-root");
            const fs::path sourceRoot = arguments.optional("--source-root", "src");
            const fs::path entry = arguments.take("--entry");
            const fs::path sourceLibraries = arguments.optional("--source-libraries");
            const auto nameSpace = arguments.take("--namespace");
            const auto numericEnvironment = arguments.optional("--native-numeric-environment");
            const bool emitCompositionFactory = arguments.flag("--composition-factory");
            arguments.finish();
            std::cout << canonical(compiledProjectInformation(prefix, manifest, expected, modelTool,
                projectRoot, sourceRoot, entry, nameSpace, numericEnvironment, emitCompositionFactory, sourceLibraries));
        } else if (command == "project-info") {
            const fs::path prefix = arguments.take("--sdk-prefix");
            const fs::path manifest = arguments.take("--sdk-manifest");
            const auto expected = arguments.take("--sdk-manifest-sha256");
            const fs::path projectRoot = arguments.take("--project-root");
            const fs::path sourceRoot = arguments.optional("--source-root", "src");
            const fs::path entry = arguments.take("--entry");
            const fs::path sourceLibraries = arguments.optional("--source-libraries");
            arguments.finish();
            std::cout << canonical(projectInformation(prefix, manifest, expected, projectRoot, sourceRoot, entry, sourceLibraries));
        } else if (command == "source-set-info") {
            const fs::path prefix = arguments.take("--sdk-prefix");
            const fs::path manifest = arguments.take("--sdk-manifest");
            const auto expected = arguments.take("--sdk-manifest-sha256");
            const fs::path descriptor = arguments.take("--source-set");
            arguments.finish();
            std::cout << canonical(sourceSetInformation(prefix, manifest, expected, descriptor));
        } else if (command == "bindings" || command == "model-bindings") {
            const bool withView = command == "bindings";
            const fs::path prefix = arguments.take("--sdk-prefix");
            const fs::path manifest = arguments.take("--sdk-manifest");
            const auto expected = arguments.take("--sdk-manifest-sha256");
            const fs::path modelTool = arguments.take("--model-compiler");
            const fs::path viewTool = withView ? arguments.take("--view-compiler") : std::string{};
            const fs::path model = arguments.optional("--model");
            const fs::path descriptor = arguments.optional("--source-set");
            const fs::path projectRoot = arguments.optional("--project-root");
            const fs::path sourceRoot = projectRoot.empty() ? fs::path{} : fs::path(arguments.optional("--source-root", "src"));
            const fs::path entry = projectRoot.empty() ? fs::path{} : fs::path(arguments.take("--entry"));
            const fs::path sourceLibraries = arguments.optional("--source-libraries");
            require(sourceLibraries.empty() || (!withView && !projectRoot.empty()),
                    "source libraries require a headless project");
            const auto inputCount = static_cast<int>(!model.empty()) + static_cast<int>(!descriptor.empty()) +
                                    static_cast<int>(!projectRoot.empty());
            require(inputCount == 1, "select exactly one --model, --source-set or --project-root");
            require(!withView || (descriptor.empty() && projectRoot.empty()),
                    "view bindings do not support source sets or projects");
            const fs::path view = withView ? arguments.take("--view") : std::string{};
            const auto nameSpace = arguments.take("--namespace");
            const fs::path output = arguments.take("--output-directory");
            const bool inspection = withView && arguments.flag("--style-inspection");
            const auto compilation = arguments.optional("--compilation", "header-only");
            require(compilation == "header-only" || compilation == "compiled", "unknown model compilation mode");
            const bool compiled = compilation == "compiled";
            require(!compiled || (!withView && !projectRoot.empty()), "compiled mode requires a headless project");
            const auto numericEnvironment = arguments.optional("--native-numeric-environment");
            require(numericEnvironment.empty() || (!withView && !projectRoot.empty()),
                    "native numeric environment requires a headless project");
            const auto expectedPlan = compiled ? arguments.take("--plan-sha256") : std::string{};
            const bool requireCompositionPlan = arguments.flag("--require-composition-plan");
            const bool emitCompositionFactory = arguments.flag("--composition-factory");
            require(!requireCompositionPlan || (!withView && !projectRoot.empty()),
                    "composition plans require a headless project");
            require(!emitCompositionFactory || (!withView && !projectRoot.empty()),
                    "composition factories require a headless project");
            arguments.finish();
            if (withView)
                generateBindings(prefix, manifest, expected, modelTool, viewTool, model, view, nameSpace, output, inspection);
            else if (compiled)
                generateCompiledProjectBindings(prefix, manifest, expected, modelTool, projectRoot, sourceRoot,
                    entry, nameSpace, output, expectedPlan, numericEnvironment,
                    requireCompositionPlan || emitCompositionFactory, emitCompositionFactory, sourceLibraries);
            else if (!projectRoot.empty())
                generateProjectBindings(prefix, manifest, expected, modelTool, projectRoot, sourceRoot, entry, nameSpace, output,
                    numericEnvironment, requireCompositionPlan || emitCompositionFactory, emitCompositionFactory, sourceLibraries);
            else if (!descriptor.empty())
                generateSourceSetBindings(prefix, manifest, expected, modelTool, descriptor, nameSpace, output);
            else
                generateModelBindings(prefix, manifest, expected, modelTool, model, nameSpace, output);
        } else {
            throw std::runtime_error("unknown SDK tool command");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-sdk-tool: " << error.what() << '\n';
        return 2;
    }
}
