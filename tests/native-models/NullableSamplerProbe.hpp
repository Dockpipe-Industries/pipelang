// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Fundamentals.hpp"
#include <string>

namespace nullable_tests {
class NullableSamplerProbe final : public pipelang_models::NullableSampler {
public:
    using Value = pipelang::model::Optional<float>;
    using Outcome = pipelang::model::Result<Value, std::string>;
    int mode = 0;
    Outcome Sample() override {
        if (mode == 0)
            return Outcome::Success(Value::Some(3.5f));
        if (mode == 1)
            return Outcome::Success(Value::None());
        if (mode == 2)
            return Outcome::Failure("domain failure");
        return Outcome::Cancelled("cancelled exactly");
    }
};
}
