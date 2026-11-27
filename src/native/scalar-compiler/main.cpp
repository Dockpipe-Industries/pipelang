// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "scalar.hpp"
#include "../sdk-tools/manifest.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        Arguments arguments(argc, argv);
        if (arguments.flag("--help")) {
            arguments.finish();
            std::cout << "pipelang-native-cpp --source FILE --output-dir ABSOLUTE_DIRECTORY\n"
                         "  --cpp-emitter ABSOLUTE_EXECUTABLE --cpp-emitter-sha256 SHA256\n"
                         "  [--profile pipelang.cpp.float64-calls.v1] [--package ID] [--module ID] [--source-sha256 SHA256]\n";
            return 0;
        }
        const fs::path source = fs::absolute(arguments.take("--source")).lexically_normal();
        const fs::path output = arguments.take("--output-dir");
        const auto package = arguments.optional("--package", "example.native");
        const auto module = arguments.optional("--module", "app.root");
        const auto selected = arguments.optional("--profile", pipelang::scalar::profile);
        const auto expectedSource = arguments.optional("--source-sha256");
        const fs::path emitter = arguments.take("--cpp-emitter");
        const auto expectedEmitter = arguments.take("--cpp-emitter-sha256");
        arguments.finish();
        require(selected == pipelang::scalar::profile, "unsupported retired scalar profile");
        require(output.is_absolute() && emitter.is_absolute(), "scalar output and selected emitter must be absolute");
        exactHash(expectedEmitter);
        require(fileHash(emitter) == expectedEmitter, "selected scalar emitter checksum differs");
        const auto bytes = readFile(source, 65536);
        const auto sourceHash = sha256(bytes);
        if (!expectedSource.empty()) {
            exactHash(expectedSource);
            require(sourceHash == expectedSource, "scalar source checksum differs");
        }
        const auto compilation = pipelang::scalar::compile(bytes, package, module);
        const auto executed = process({emitter.string()}, compilation.request.dump(), std::chrono::seconds(45), 32U << 20);
        require(executed.exit == 0 && executed.diagnostic.empty(), "selected scalar emitter failed: " + executed.diagnostic);
        const auto response = parseJson(executed.output, 32U << 20);
        exactObject(response, {"contract", "profile", "source", "functions"}, {}, "scalar emitter response");
        require(response.at("contract") == "pipelang.cpp-emitter.response.v4" && response.at("profile") == selected, "scalar emitter protocol differs");
        const auto generated = text(response.at("source"), "generated scalar source", 16U << 20);
        const auto& projected = compilation.request.at("functions");
        const auto& bindings = response.at("functions");
        require(bindings.is_array() && bindings.size() == projected.size(), "scalar emitter binding count differs");
        for (std::size_t index = 0; index < projected.size(); ++index) {
            const auto& function = projected[index];
            const auto path = function.at("path").get<std::string>();
            require(bindings[index] == Json({{"package_id", package}, {"path", path},
                {"name", "fn_" + sha256(package + '\0' + path).substr(0, 24)}}), "scalar emitter changed a semantic binding");
        }
        const auto self = executablePath();
        require(fs::file_size(self) <= 256U << 20, "scalar executable identity exceeds capacity");
        const Json metadata{{"contract", "pipelang.native-cpp-export.v1"}, {"profile", selected}, {"language_contract", "v0.113.0"},
            {"compiler_contract", "pipelang.compiler.v1"}, {"source_sha256", sourceHash}, {"generated_sha256", sha256(generated)},
            {"compiler_executable_sha256", fileHash(self)}, {"emitter_executable_sha256", expectedEmitter}, {"functions", compilation.functions}};
        require(fileHash(source) == sourceHash && fileHash(emitter) == expectedEmitter, "scalar inputs changed during compilation");
        publishOwnedDirectory(output.lexically_normal(), {{"generated.hpp", generated}, {"bindings.json", canonical(metadata)}}, {"generated.hpp", "bindings.json"});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-native-cpp: " << error.what() << '\n';
        return 2;
    }
}
