// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_INHERITANCE_CPP_REFERENCE
#include "ClassInheritance.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <iostream>
#include <stdexcept>

#ifndef PIPELANG_INHERITANCE_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_inherited;
#endif

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try {
#ifdef PIPELANG_INHERITANCE_CPP_REFERENCE
        reference::App model;
        require(model.Fixed() == 3 && model.InheritedOverload() == 7, "reference inherited overload failed");
        require(model.ViaThis() == 115 && model.Read() == 115, "reference virtual dispatch failed");
#else
        pipelang::model::TaskScope scope;
        const std::array<HostedConstructorArgument, 1> initial{std::int64_t{4}};
        auto instance = HostedInstance::bind(proof_inherited, initial);
        require(instance.is_success(), "native inherited binding failed");
        const auto fixed = instance.value().start(scope, {}).result();
        const std::array<HostedCallArgument, 1> argument{std::int64_t{4}};
        const auto overload = instance.value().startMethod(scope, "Fixed(int)", argument).result();
        const auto dynamic = instance.value().startMethod(scope, "ViaThis", {}).result();
        const auto read = instance.value().startMethod(scope, "Read", {}).result();
        require(fixed.is_success() && fixed.value() == 3 && overload.is_success() && overload.value().integer() == 4,
                "native inherited overload failed");
        require(dynamic.is_success() && dynamic.value().integer() == 115 && read.is_success() && read.value().integer() == 115,
                "native inherited call lost the concrete receiver");
#endif
        std::cout << "Inherited methods keep declaring state, overloads and virtual dispatch.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
