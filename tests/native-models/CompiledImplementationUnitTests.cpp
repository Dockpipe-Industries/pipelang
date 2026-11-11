// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompiledImplementationUnitTests.hpp"
#include "../../src/native/model-compiler/CompiledImplementationUnits.hpp"
#include "../../src/native/support/support.hpp"

namespace {

using pipelang::models::CompiledImplementationUnits;
using pipelang::tools::require;

std::string unit(const std::string& contents) {
    return "// Generated project implementation.\n"
        "#include \"Objects.hpp\"\nnamespace split_fixture {\n" + contents + "}\n";
}

}

void compiledImplementationUnitContracts() {
    CompiledImplementationUnits grouped("split_fixture");
    grouped.addOwner("First");
    grouped.addOwner("Second");
    std::string expected;
    for (std::size_t index = 0; index < CompiledImplementationUnits::preferredDefinitions; ++index) {
        const auto definition = "void Function" + std::to_string(index) + "() {}\n";
        grouped.append("First", definition);
        expected += definition;
    }
    grouped.append("First", "void Overflow() {}\n");
    grouped.append("Second", "void Peer() {}\n");
    const auto groupedFiles = grouped.files();
    require(groupedFiles.size() == 3 &&
            groupedFiles.at("Implementation_0.cpp") == unit(expected) &&
            groupedFiles.at("Implementation_1.cpp") == unit("void Peer() {}\n") &&
            groupedFiles.at("Implementation_2.cpp") == unit("void Overflow() {}\n"),
            "function count grouping changed contents, owner membership or deterministic ordinals");

    CompiledImplementationUnits sized("split_fixture");
    sized.addOwner("Owner");
    const auto exact = std::string(CompiledImplementationUnits::preferredBodyBytes, 'x');
    const auto oversized = exact + "oversized";
    sized.append("Owner", exact);
    sized.append("Owner", oversized);
    sized.append("Owner", "following");
    const auto sizedFiles = sized.files();
    require(sizedFiles.size() == 3 &&
            sizedFiles.at("Implementation_0.cpp") == unit(exact) &&
            sizedFiles.at("Implementation_1.cpp") == unit(oversized) &&
            sizedFiles.at("Implementation_2.cpp") == unit("following"),
            "byte grouping split, changed or combined an oversized complete definition");

    CompiledImplementationUnits bounded("split_fixture");
    for (std::size_t index = 0; index < pipelang::models::resourceBudget().implementationUnits; ++index)
        bounded.addOwner("Owner" + std::to_string(index));
    bounded.append("Owner0", exact);
    const auto before = bounded.files();
    bool refused = false;
    try {
        bounded.append("Owner0", "next");
    } catch (const pipelang::models::Diagnostic& error) {
        refused = std::string(error.what()) == "compiled project implementation unit limit exceeded";
    }
    require(refused && bounded.files() == before,
            "implementation limit changed or partially published an overflowing definition");
}
