// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeTextPool.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace pipelang::native_app_v19;

int main()
{
    try {
        NativeTextPool pool(1024, "text test storage limit");
        const auto first = pool.retain("first");
        for (int attempt = 0; attempt < 8192; ++attempt) {
            const auto repeated = pool.retain("first");
            if (first.bytes != repeated.bytes || first.size != repeated.size)
                throw std::runtime_error("duplicate text did not reuse its existing storage");
        }
        for (int index = 0; index < 1024; ++index) {
            try {
                pool.retain(std::to_string(index));
            } catch (const pipelang::model::EvaluationLimitFailure& failure) {
                if (std::string_view(failure.what()) != "text test storage limit" ||
                    std::string_view(first.bytes, first.size) != "first")
                    throw std::runtime_error("budget refusal damaged prior text");
                std::cout << "Native text pool bounded bookkeeping and retained prior values.\n";
                return 0;
            }
        }
        throw std::runtime_error("text pool failed to enforce its byte budget");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
