// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdexcept>

namespace pipelang::sqlite {
class Failure final : public std::runtime_error {
public:
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};
}
