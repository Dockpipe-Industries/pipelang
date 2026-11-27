// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_wide_methods;

int main()
{
    try {
        const auto binding = native::HostedInstance::bind(proof_wide_methods, {});
        if (!binding.is_success())
            throw std::runtime_error(binding.error());

        model::TaskScope scope;
        constexpr std::array methods{
            std::pair{"Method000", 0}, std::pair{"Method032", 32}, std::pair{"Method255", 255}
        };
        for (const auto& [method, expected] : methods) {
            const auto result = binding.value().startMethod(scope, method, {}).result();
            if (!result.is_success())
                throw std::runtime_error(result.error());
            if (result.value().integer() != expected)
                throw std::runtime_error(std::string("wrong result for ") + method);
        }
        std::cout << "Native binding exposed the full 256-method class.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
