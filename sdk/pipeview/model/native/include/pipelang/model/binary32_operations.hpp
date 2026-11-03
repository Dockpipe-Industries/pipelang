// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "binary32.hpp"
#include "binary32_operands.hpp"

namespace pipelang::model {
// Every Source binary32 arithmetic node retains its materialized rounding
// boundary, even when the native compiler permits fused multiply-add.
inline float addBinary32(Binary32Operands operands) {
    volatile float result = operands.first + operands.second;
    return result;
}
inline float subtractBinary32(Binary32Operands operands) {
    volatile float result = operands.first - operands.second;
    return result;
}
inline float multiplyBinary32(Binary32Operands operands) {
    volatile float result = operands.first * operands.second;
    return result;
}
inline float divideBinary32(Binary32Operands operands) {
    volatile float result = operands.first / operands.second;
    return result;
}
}
