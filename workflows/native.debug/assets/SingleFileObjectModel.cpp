// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <iostream>
#include <stdexcept>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_single_file_object_model;

namespace {

native::HostedValue checked(const native::HostedValueResult& result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return result.value();
}

}

int main()
{
    try {
        const auto bound = native::HostedInstance::bind(proof_single_file_object_model, {});
        if (!bound.is_success())
            throw std::runtime_error(bound.error());
        const auto& instance = bound.value();
        model::TaskScope tasks;

        if (checked(instance.startMethod(tasks, "Read", {}).result()).integer() != 115)
            throw std::runtime_error("the original single-file model lost inherited dispatch");
        if (!checked(instance.startMethod(tasks, "Identity", {}).result()).boolean())
            throw std::runtime_error("the original single-file model lost receiver identity");
        const auto first = checked(instance.startMethod(tasks, "Reference", {}).result());
        const auto second = checked(instance.startMethod(tasks, "Reference", {}).result());
        if (!first.sameObject(second))
            throw std::runtime_error("single-file model binding did not preserve an exported object reference");
        std::cout << "Native object binding passed on the original single-file source.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
