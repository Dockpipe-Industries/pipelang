// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_COMPOSITION_CPP_REFERENCE
#include "Composition.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <iostream>
#include <stdexcept>

#ifndef PIPELANG_COMPOSITION_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_composition_default;
extern "C" const HostedModel proof_composition_initial;
#endif

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try {
#ifdef PIPELANG_COMPOSITION_CPP_REFERENCE
        reference::Composition empty;
        reference::Composition seeded(37);
        require(empty.snapshot().Seed == 0 && seeded.snapshot().Seed == 37, "reference seed mismatch");
        require(seeded.Evaluate() == 17 && seeded.Floating() == 1.5 && seeded.IndependentDefaults(),
                "reference result mismatch");
        require(seeded.Add(std::int64_t{40}) == 42 && seeded.Add(1.0) == 1.5, "reference overload mismatch");
#else
        pipelang::model::TaskScope scope;
        auto empty = HostedInstance::bind(proof_composition_default, {});
        const std::array<HostedConstructorArgument, 1> args{std::int64_t{37}};
        auto seeded = HostedInstance::bind(proof_composition_initial, args);
        require(empty.is_success() && seeded.is_success(), "native constructor binding failed");
        require(HostedInstance::bind(proof_composition_initial, {}).is_failure(), "missing constructor argument admitted");
        require(HostedInstance::bind(proof_composition_default, args).is_failure(), "extra constructor argument admitted");
        require(empty.value().snapshot().value().field("Seed").integer() == 0 &&
                seeded.value().snapshot().value().field("Seed").integer() == 37, "native seed mismatch");
        const auto evaluated = seeded.value().start(scope, {}).result();
        const auto floating = seeded.value().startMethod(scope, "Floating", {}).result();
        const auto defaults = seeded.value().startMethod(scope, "IndependentDefaults", {}).result();
        require(evaluated.is_success() && evaluated.value() == 17 &&
                floating.is_success() && floating.value().floating() == 1.5 &&
                defaults.is_success() && defaults.value().boolean(), "native methods did not match the source");
        const std::array<HostedCallArgument, 1> integers{std::int64_t{40}};
        const std::array<HostedCallArgument, 1> floats{HostedValue::fromFloat(1.0)};
        const auto integralOverload = seeded.value().startMethod(scope, "Add(int)", integers).result();
        const auto floatingOverload = seeded.value().startMethod(scope, "Add(float)", floats).result();
        require(integralOverload.is_success() && integralOverload.value().integer() == 42 &&
                floatingOverload.is_success() && floatingOverload.value().floating() == 1.5, "native overload result mismatch");
        require(seeded.value().startMethod(scope, "Add(int)", floats).result().is_failure(), "wrong overload argument admitted");
#endif
        std::cout << "Both source constructors preserve their own state and generic method behavior.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
