// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_ARRAY_CPP_REFERENCE
#include "InitialArray.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <span>
#include <stdexcept>

#ifdef PIPELANG_ARRAY_CPP_REFERENCE
using reference::source::InitialArray::Model;
#else
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_initial_array;
#endif

void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

int main()
{
    try {
        const std::array<std::uint8_t, 4> receiverBytes{195, 169, 0, 33};
        const std::array<std::uint8_t, 3> cellBytes{0, 97, 255};
        const std::array<std::uint8_t, 4> savedBytes{0, 97, 255, 1};
        constexpr std::int64_t growthCount = 12000;
        constexpr std::size_t growthSteps = 262144;
        const auto allGrowthBytes = [](std::span<const std::uint8_t> bytes) {
            return bytes.size() == growthCount && std::ranges::all_of(bytes, [](auto value) { return value == 255; });
        };
#ifdef PIPELANG_ARRAY_CPP_REFERENCE
        for (const auto length : {std::int64_t{-1}, std::int64_t{10000000}}) {
            bool rejected = false;
            try {
                Model invalid(length, "initial");
            } catch (const pipelang::model::ExecutionFailure& error) {
                rejected = std::string_view(error.what()) == "array size outside admitted range";
            }
            require(rejected, "reference admitted an invalid array length");
        }
        bool rejectedSlice = false;
        try {
            Model invalid(2, "split");
        } catch (const pipelang::model::ExecutionFailure& error) {
            rejectedSlice = std::string_view(error.what()) == "UTF-8 slice end splits a scalar";
        }
        require(rejectedSlice, "reference admitted a split UTF-8 scalar");
        bool rejectedBytes = false;
        try {
            Model invalid(2, "invalid-bytes");
        } catch (const pipelang::model::ExecutionFailure& error) {
            rejectedBytes = std::string_view(error.what()) == "string contains invalid UTF-8";
        }
        require(rejectedBytes, "reference decoded invalid UTF-8 during binding");
        pipelang::model::TaskOptions growthOptions;
        growthOptions.initializationSteps = growthSteps;
        const Model grown(growthCount, "grow", growthOptions);
        require(allGrowthBytes(grown.ReadBytes().values()), "reference constructor lost incrementally written bytes");
        Model empty(0, "initial");
        require(empty.snapshot().cells.count() == 0, "reference evaluated the unselected constructor branch");
        Model model(2, "initial");
        const auto initial = model.snapshot();
        require(initial.total == 84 && model.Read().number == 43 && model.Read().text == "constructed",
                "reference constructor altered its iteration snapshot");
        require(std::ranges::equal(model.ReadBytes().values(), receiverBytes) &&
                std::ranges::equal(initial.bytes.values(), receiverBytes) &&
                std::ranges::equal(model.Read().payload.values(), cellBytes) &&
                std::ranges::equal(model.ReadSaved().payload.values(), savedBytes),
                "reference constructor lost a byte buffer or changed an alias");
        require(model.Edit(17).is_success(), "reference array mutation failed");
        const auto current = model.snapshot().cells;
        require(initial.cells.at(0).number == 43 && current.at(0).number == 17 &&
                current.at(1).text == "initial2", "reference array snapshot lost independence");
#else
        const auto text = HostedValue::fromText("initial");
        const std::array<HostedConstructorArgument, 2> growthArguments{growthCount, HostedValue::fromText("grow")};
        const auto grown = HostedInstance::bind(proof_initial_array, growthArguments, growthSteps);
        require(grown.is_success(), grown.is_success() ? "" : grown.error());
        pipelang::model::TaskScope growthScope;
        const auto grownBytes = grown.value().startMethod(growthScope, "ReadBytes", {}).result();
        require(grownBytes.is_success() && allGrowthBytes(grownBytes.value().bytes()),
                "native constructor lost incrementally written bytes");
        for (const auto length : {std::int64_t{-1}, std::int64_t{10000000}}) {
            const std::array<HostedConstructorArgument, 2> invalid{length, text};
            const auto rejected = HostedInstance::bind(proof_initial_array, invalid);
            require(rejected.is_failure() && rejected.error() == "array size outside admitted range",
                    "invalid construction array size did not fail before binding");
        }
        const std::array<HostedConstructorArgument, 2> splitArguments{
            std::int64_t{2}, HostedValue::fromText("split")};
        const auto split = HostedInstance::bind(proof_initial_array, splitArguments);
        require(split.is_failure() && split.error() == "UTF-8 slice end splits a scalar",
                "native constructor admitted a split UTF-8 scalar");
        const std::array<HostedConstructorArgument, 2> invalidBytes{
            std::int64_t{2}, HostedValue::fromText("invalid-bytes")};
        const auto invalid = HostedInstance::bind(proof_initial_array, invalidBytes);
        require(invalid.is_failure() && invalid.error() == "string contains invalid UTF-8",
                "native constructor decoded invalid UTF-8 during binding");
        const std::array<HostedConstructorArgument, 2> emptyArguments{std::int64_t{0}, text};
        const auto empty = HostedInstance::bind(proof_initial_array, emptyArguments);
        require(empty.is_success() && empty.value().snapshot().value().field("cells").size() == 0,
                "native constructor evaluated the unselected branch");
        const std::array<HostedConstructorArgument, 2> arguments{std::int64_t{2}, text};
        const auto instance = HostedInstance::bind(proof_initial_array, arguments);
        require(instance.is_success(), instance.is_success() ? "" : instance.error());
        const auto& model = instance.value();
        pipelang::model::TaskScope scope;
        const auto initial = model.snapshot().value();
        const auto read = model.startValue(scope, {}).result();
        require(read.is_success() && read.value().field("number").integer() == 43 &&
                read.value().field("text").text() == "constructed" && initial.field("total").integer() == 84,
                "native constructor altered its iteration snapshot");
        const auto bytes = model.startMethod(scope, "ReadBytes", {}).result();
        const auto saved = model.startMethod(scope, "ReadSaved", {}).result();
        require(bytes.is_success() && saved.is_success() &&
                std::ranges::equal(bytes.value().bytes(), receiverBytes) &&
                std::ranges::equal(initial.field("bytes").bytes(), receiverBytes) &&
                std::ranges::equal(read.value().field("payload").bytes(), cellBytes) &&
                std::ranges::equal(saved.value().field("payload").bytes(), savedBytes),
                "native constructor lost a byte buffer or changed an alias");
        const std::array<HostedCallArgument, 1> edit{std::int64_t{17}};
        require(model.startMethod(scope, "Edit", edit).result().is_success(), "cannot edit constructor collection");
        const auto current = model.snapshot().value().field("cells");
        require(initial.field("cells").at(0).field("number").integer() == 43 &&
                current.at(0).field("number").integer() == 17 && current.at(1).field("text").text() == "initial2",
                "native constructor collection snapshots lost independence");
#endif
        std::cout << "Hosted constructors preserve Array bounds, snapshot iteration and owned values.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
