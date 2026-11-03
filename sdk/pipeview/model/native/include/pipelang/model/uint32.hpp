// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"

namespace pipelang::model {

static_assert(std::numeric_limits<std::uint32_t>::digits == 32);

inline std::uint32_t add(std::uint32_t left, std::uint32_t right) {
    const auto result = static_cast<std::uint64_t>(left) + right;
    if (result > std::numeric_limits<std::uint32_t>::max())
        throw ExecutionFailure("uint32 addition overflow");
    return static_cast<std::uint32_t>(result);
}

inline std::uint32_t subtract(std::uint32_t left, std::uint32_t right) {
    if (left < right)
        throw ExecutionFailure("uint32 subtraction underflow");
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) - right);
}

inline std::uint32_t multiply(std::uint32_t left, std::uint32_t right) {
    const auto result = static_cast<std::uint64_t>(left) * right;
    if (result > std::numeric_limits<std::uint32_t>::max())
        throw ExecutionFailure("uint32 multiplication overflow");
    return static_cast<std::uint32_t>(result);
}

inline std::uint32_t divide(std::uint32_t left, std::uint32_t right) {
    if (right == 0)
        throw ExecutionFailure("uint32 division by zero");
    return static_cast<std::uint32_t>(left / right);
}

inline std::uint32_t remainder(std::uint32_t left, std::uint32_t right) {
    if (right == 0)
        throw ExecutionFailure("uint32 remainder by zero");
    return static_cast<std::uint32_t>(left % right);
}

inline std::uint32_t wrappingAdd(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) + right);
}

inline std::uint32_t wrappingSubtract(std::uint32_t left, std::uint32_t right) {
    // Unsigned64 subtraction is defined modulo 2^64; subsequent narrowing is
    // modulo 2^32, including when the mathematical difference is negative.
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) - right);
}

inline std::uint32_t wrappingMultiply(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) * right);
}

inline std::uint32_t bitAnd(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(left & right);
}

inline std::uint32_t bitOr(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(left | right);
}

inline std::uint32_t bitXor(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(left ^ right);
}

inline std::uint32_t bitNot(std::uint32_t value) {
    return static_cast<std::uint32_t>(value ^ std::numeric_limits<std::uint32_t>::max());
}

inline void requireUInt32ShiftCount(std::int64_t count) {
    if (count < 0 || count > 31)
        throw ExecutionFailure("uint32 shift count must be between 0 and 31");
}

inline std::uint32_t shiftLeft(std::uint32_t value, std::int64_t count) {
    requireUInt32ShiftCount(count);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) << static_cast<unsigned>(count));
}

inline std::uint32_t shiftRight(std::uint32_t value, std::int64_t count) {
    requireUInt32ShiftCount(count);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> static_cast<unsigned>(count));
}

inline std::uint32_t checkedUInt32(std::int64_t value) {
    if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max())
        throw ExecutionFailure("uint32 conversion requires an int in range 0..4294967295");
    return static_cast<std::uint32_t>(value);
}

inline std::int64_t widenUInt32(std::uint32_t value) {
    return static_cast<std::int64_t>(value);
}

}
