// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ScalarFunction.hpp"
#include "Target.hpp"
#include "ApplicationLink.hpp"
#include "../composition.hpp"
#include "../CompositionPlanner.hpp"
#include "../CompositionFactoryProfile.hpp"
#include "../../sdk-tools/SourceProject.hpp"
#include "../../sdk-tools/source_set.hpp"

#include <algorithm>
#include <iostream>

namespace {

using namespace pipelang;
namespace fs = std::filesystem;

const models::ModuleSource* findSource(const std::vector<models::ModuleSource>& sources,
                                      std::string_view path)
{
    const auto source = std::find_if(sources.begin(), sources.end(), [path](const auto& fragment) {
        return fragment.path == path;
    });
    return source == sources.end() ? nullptr : &*source;
}

void requirePublicSymbol(std::string_view symbol, std::string_view option)
{
    constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
    constexpr std::string_view characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
    tools::require(!symbol.empty() && symbol.size() <= 128 && letters.find(symbol.front()) != std::string_view::npos &&
        symbol.find_first_not_of(characters) == std::string_view::npos,
        std::string(option) + " requires a C identifier with at most 128 characters");
    tools::require(symbol != "main" && !symbol.starts_with("pipelang_native_"),
        std::string(option) + " must not use a native runtime symbol");
}

}

int main(int argc, char** argv)
{
    fs::path sourceFile;
    std::vector<models::ModuleSource> standalone;
    std::optional<tools::SourceProject> project;
    std::optional<tools::SourceSet> sourceSet;
    try {
        tools::Arguments arguments(argc, argv);
        sourceFile = arguments.optional("--source");
        const fs::path modelFile = arguments.optional("--model");
        auto modelExports = arguments.repeated("--model-export");
        const fs::path projectRoot = arguments.optional("--project-root");
        const fs::path sourceSetPath = arguments.optional("--source-set");
        const fs::path sourceRoot = arguments.optional("--source-root");
        const fs::path projectEntry = arguments.optional("--project-entry");
        const fs::path sourceLibraries = arguments.optional("--source-libraries");
        const auto entry = arguments.take("--entry");
        const fs::path output = arguments.take("--output");
        const fs::path requestedLlc = arguments.take("--llc");
        const auto profile = arguments.optional("--profile", "debug");
        const auto numericEnvironments = arguments.repeated("--native-numeric-environment");
        const bool executable = arguments.flag("--executable");
        const bool runtime = arguments.flag("--runtime") || executable;
        const bool sourceTrace = arguments.flag("--source-trace");
        const fs::path linkSdk = arguments.optional("--link-sdk");
        const auto linkSdkDigest = arguments.optional("--link-sdk-sha256");
        const fs::path linker = arguments.optional("--linker");
        const fs::path symbols = arguments.optional("--pdb");
        models::native_ir::MacApplicationTools mac{
            arguments.optional("--macos-sysroot"), arguments.optional("--macos-sdk-version"),
            arguments.optional("--dsymutil")};
        const auto hostSymbol = arguments.optional("--host-symbol");
        const auto numericSymbols = arguments.repeated("--numeric-symbol");
        const auto hostConstructor = arguments.optional("--host-constructor");
        const auto hostComposition = arguments.optional("--host-composition");
        const bool exposeSnapshot = arguments.flag("--host-snapshot");
        const bool allowMultipleTasks = arguments.flag("--host-multiple-tasks");
        const bool exposeObjectReferences = arguments.flag("--host-object-references");
        const auto hostMethods = arguments.repeated("--host-method");
        const auto requestedTarget = arguments.optional("--target", "host");
        const auto targetName = requestedTarget == "host"
            ? std::string(models::native_ir::hostTargetName()) : requestedTarget;
        const auto& target = models::native_ir::findTarget(targetName);
        arguments.finish();

        const auto inputCount = static_cast<int>(!sourceFile.empty()) +
            static_cast<int>(!projectRoot.empty()) + static_cast<int>(!sourceSetPath.empty()) +
            static_cast<int>(!modelFile.empty());
        tools::require(inputCount == 1, "select exactly one --source, --model, --project-root or --source-set");
        if (!modelFile.empty())
            sourceFile = modelFile;
        if (projectRoot.empty()) {
            tools::require(sourceRoot.empty() && projectEntry.empty() && sourceLibraries.empty(),
                "--source-root, --project-entry and --source-libraries require --project-root");
        } else {
            tools::require(!projectEntry.empty(), "--project-root requires --project-entry");
        }
        std::optional<models::native_ir::ApplicationLink> applicationLink;
        if (executable) {
            applicationLink.emplace(target, linkSdk, linkSdkDigest, linker, output, symbols, std::move(mac));
        } else {
            tools::require(linkSdk.empty() && linkSdkDigest.empty() && linker.empty() && symbols.empty(),
                "--link-sdk, --link-sdk-sha256, --linker and --pdb require --executable");
            tools::require(mac.sysroot.empty() && mac.sdkVersion.empty() && mac.dsymutil.empty(),
                "--macos-sysroot, --macos-sdk-version and --dsymutil require --executable");
            tools::require(output.extension() == target.extension,
                "native object output for " + targetName + " must use " + std::string(target.extension));
        }
        tools::require(profile == "debug" || profile == "release", "native profile must be debug or release");
        tools::require(numericSymbols.size() <= 1, "duplicate argument --numeric-symbol");
        const bool numeric = !numericSymbols.empty();
        tools::require(numericEnvironments.size() <= 1, "duplicate argument --native-numeric-environment");
        const bool activeHostFloating = !numericEnvironments.empty();
        tools::require(!activeHostFloating || numericEnvironments.front() == "active-host",
            "native numeric environment must be active-host when specified");
        tools::require(!activeHostFloating || (targetName == "linux-x64" && (runtime || !hostSymbol.empty() || numeric)),
            "active-host floating point requires a Linux x64 native runtime or hosted object or numeric export");
        tools::require(!numeric || (activeHostFloating && !runtime && hostSymbol.empty()),
            "--numeric-symbol requires a separate active-host numeric object");
        tools::require(requestedLlc.is_absolute(), "--llc must be an absolute LLVM executable path");
        tools::require(modelFile.empty() || !hostSymbol.empty(), "--model requires --host-symbol");
        tools::require(modelExports.empty() || !modelFile.empty(), "--model-export requires --model");
        const bool explicitObjectModel = !modelExports.empty();
        tools::require(modelFile.empty() || hostComposition.empty(), "native hosted composition requires project source");
        tools::require(!runtime || hostSymbol.empty(), "select either --runtime or --host-symbol");
        tools::require(!sourceTrace || runtime, "--source-trace requires the native runtime");
        tools::require(hostMethods.empty() || !hostSymbol.empty(), "--host-method requires --host-symbol");
        tools::require(hostConstructor.empty() || !hostSymbol.empty(), "--host-constructor requires --host-symbol");
        tools::require(hostConstructor.empty() ||
            (hostConstructor.size() <= 1024 && hostConstructor.front() == '(' && hostConstructor.back() == ')'),
            "--host-constructor requires a parenthesized parameter type list, such as (int)");
        tools::require(!exposeSnapshot || !hostSymbol.empty(), "--host-snapshot requires --host-symbol");
        tools::require(!allowMultipleTasks || !hostSymbol.empty(), "--host-multiple-tasks requires --host-symbol");
        tools::require(!exposeObjectReferences || !hostSymbol.empty(), "--host-object-references requires --host-symbol");
        tools::require(hostComposition.empty() || (!hostSymbol.empty() && exposeObjectReferences),
            "--host-composition requires --host-symbol and --host-object-references");
        if (!hostSymbol.empty())
            requirePublicSymbol(hostSymbol, "--host-symbol");
        if (numeric)
            requirePublicSymbol(numericSymbols.front(), "--numeric-symbol");
        tools::require((!runtime && hostSymbol.empty()) || !target.runtimeTriple.empty(),
            "the native application runtime is unavailable for " + targetName);

        if (!sourceSetPath.empty()) {
            tools::require(hostComposition.empty(), "native hosted composition requires project source");
            sourceSet.emplace(tools::SourceSet::load(sourceSetPath));
            standalone = sourceSet->sourceModules();
        } else if (projectRoot.empty()) {
            tools::require(sourceFile.extension() == ".pipe", "native input must be a .pipe file");
            auto& fragment = standalone.emplace_back();
            fragment.path = sourceFile.filename().string();
            fragment.source = tools::readFile(sourceFile, 64U << 10);
            auto header = models::parseModuleHeader(fragment.source);
            fragment.name = header.module.empty()
                ? models::moduleIdentityFromStem(sourceFile.stem().string()) : std::move(header.module);
        } else {
            project = tools::SourceProject::load(projectRoot, sourceRoot.empty() ? fs::path("src") : sourceRoot,
                                                 projectEntry, sourceLibraries);
        }

        const auto& sources = project ? project->modules() : standalone;
        const auto checked = [&] {
            if (sourceSet) {
                auto exports = sourceSet->exportedModels();
                const bool objectModel = !exports.empty();
                auto program = models::linkModules(sources, sourceSet->entryModule(), objectModel);
                program.activeHostNumericEnvironment = numeric;
                return models::CheckedProgram::complete(std::move(program), objectModel, std::move(exports));
            }
            if (!modelFile.empty()) {
                const auto& fragment = standalone.front();
                auto program = models::parse(fragment.source, explicitObjectModel);
                if (!program.module.empty() || !program.imports.empty()) {
                    const auto* message = explicitObjectModel ? "v3 module/import declarations require a closed modules request"
                                                      : "module and import declarations require pipelang.native-model.v2";
                    throw models::Diagnostic(message, 0);
                }
                for (auto& declaration : program.declarations) {
                    declaration.sourceName = declaration.name;
                    declaration.originPath = fragment.path;
                }
                program.activeHostNumericEnvironment = numeric;
                return models::CheckedProgram::complete(std::move(program), explicitObjectModel, std::move(modelExports));
            }
            const auto& module = project ? project->entryModule() : standalone.front().name;
            return models::CheckedProgram::project(sources, module, numeric);
        }();
        const auto& program = checked.definition();
        const auto compositions = program.compositions.empty() ? std::vector<models::CompositionGraph>{}
            : models::planCompositions(program);
        const models::CompositionGraph* composition = nullptr;
        if (!hostComposition.empty()) {
            const auto found = std::find_if(compositions.begin(), compositions.end(), [&](const auto& candidate) {
                return candidate.name == hostComposition;
            });
            tools::require(found != compositions.end(), "native hosted composition was not found");
            composition = &*found;
            const std::vector<models::CompositionGraph> selected{*composition};
            models::CompositionFactoryProfile(program, selected).admit();
        }

        const auto verifySourceSetOutputs = [&] {
            if (!sourceSet)
                return;

            const auto& inputs = sourceSet->dependencies();
            const auto isInput = [&](const fs::path& destination) {
                if (destination.empty())
                    return false;
                return std::ranges::find(inputs, fs::weakly_canonical(fs::absolute(destination))) != inputs.end();
            };
            tools::require(!isInput(output) && !isInput(symbols),
                "native output cannot overwrite a source-set input");
        };
        verifySourceSetOutputs();

        const auto lowered = [&] {
            if (numeric)
                return models::native_ir::lowerNumericProgram(checked, entry, numericSymbols.front());
            if (!hostSymbol.empty())
                return models::native_ir::lowerHostedProgram(checked, entry, hostSymbol, hostMethods, {
                    .exposesSnapshot = exposeSnapshot, .allowsMultipleTasks = allowMultipleTasks,
                    .exposesObjectReferences = exposeObjectReferences, .constructorSignature = hostConstructor,
                    .composition = composition, .activeHostFloating = activeHostFloating,
                    .requiresExportedModel = explicitObjectModel
                });
            if (runtime)
                return models::native_ir::lowerRuntimeProgram(checked, entry);
            return models::native_ir::lowerScalarProgram(checked, entry);
        }();
        const bool debug = profile == "debug";
        const auto llvmIr = models::native_ir::emitLlvmIr(lowered, sources, target, debug, sourceTrace, activeHostFloating);
        const auto llc = tools::nativeExecutable(requestedLlc);
        std::vector<std::string> codeGenerator{llc.string(), debug ? "-O0" : "-O2", "-filetype=obj", "-o", "-"};
        if ((!hostSymbol.empty() || numeric) && target.format == models::native_ir::ObjectFormat::Elf)
            codeGenerator.push_back("-relocation-model=pic");
        const auto object = tools::process(codeGenerator, llvmIr, std::chrono::seconds(30), 2U << 20);

        tools::require(object.exit == 0, "LLVM object emission failed for " + targetName + ": " + object.diagnostic);
        tools::require(object.diagnostic.empty(), "LLVM reported an object emission diagnostic: " + object.diagnostic);
        tools::require(models::native_ir::matchesObjectTarget(object.output, target),
            "LLVM did not produce a relocatable object for " + targetName);
        const auto verifySources = [&] {
            if (project) {
                project->verifyUnchanged();
            } else if (sourceSet) {
                sourceSet->verifyUnchanged();
                verifySourceSetOutputs();
            } else {
                tools::require(tools::readFile(sourceFile, 64U << 10) == standalone.front().source,
                    "native source changed during compilation");
            }
        };
        if (applicationLink) {
            applicationLink->publish(object.output, verifySources);
        } else {
            verifySources();
            tools::atomicWrite(output, object.output);
        }
        return 0;
    } catch (const models::Diagnostic& error) {
        const auto& sources = project ? project->modules() : standalone;
        const auto* fragment = findSource(sources, error.path);
        if (!fragment && !sourceFile.empty() && !standalone.empty())
            fragment = &standalone.front();
        if (fragment) {
            std::cerr << (sourceFile.empty() ? fragment->path : sourceFile.string()) << ':'
                      << models::native_ir::sourceLine(fragment->source, error.offset) << ": ";
        } else {
            std::cerr << "pipelang-objectc: ";
        }
        std::cerr << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "pipelang-objectc: " << error.what() << '\n';
    }

    return 1;
}
