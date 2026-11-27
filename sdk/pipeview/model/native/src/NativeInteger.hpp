// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/result.hpp>

namespace pipelang::native_app_v19 {

inline std::int64_t checkedInteger(IntegerOperation operation, std::int64_t left, std::int64_t right)
{
    switch (operation) {
    case IntegerOperation::Add: return model::add(left, right);
    case IntegerOperation::Subtract: return model::subtract(left, right);
    case IntegerOperation::Multiply: return model::multiply(left, right);
    case IntegerOperation::Divide: return model::divide(left, right);
    case IntegerOperation::Remainder: return model::remainder(left, right);
    }
    throw model::ExecutionFailure("unknown native integer operation");
}

}
