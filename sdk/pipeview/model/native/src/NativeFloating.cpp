// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeFloating.hpp"

#include "NativeHostedConstruction.hpp"

#include <pipelang/model/binary32.hpp>
#include <pipelang/model/floating_environment.hpp>
#include <pipelang/model/numeric.hpp>

#include <bit>
#include <cmath>
#include <type_traits>

namespace pipelang::native_app_v19 {
namespace {

template<class Float>
using FloatWord = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;

template<class Float>
Float fromWord(std::uint64_t bits)
{
    return std::bit_cast<Float>(static_cast<FloatWord<Float>>(bits));
}

template<class Float>
std::uint64_t toWord(Float value)
{
    return std::bit_cast<FloatWord<Float>>(value);
}

template<class Float>
std::uint64_t evaluate(FloatingBuiltin operation, std::uint64_t first, std::uint64_t second, std::uint64_t third)
{
    using Op = FloatingBuiltin;
    const auto value = fromWord<Float>(first);
    const auto other = fromWord<Float>(second);
    const auto last = fromWord<Float>(third);
    switch (operation) {
    case Op::IsFinite: return std::isfinite(value);
    case Op::Abs: return toWord(std::abs(value));
    case Op::Sqrt: return toWord(std::sqrt(value));
    case Op::Log: return toWord(std::log(value));
    case Op::Sin: return toWord(std::sin(value));
    case Op::Cos: return toWord(std::cos(value));
    case Op::Tan: return toWord(std::tan(value));
    case Op::Asin: return toWord(std::asin(value));
    case Op::Acos: return toWord(std::acos(value));
    case Op::Atan: return toWord(std::atan(value));
    case Op::Atan2: return toWord(std::atan2(value, other));
    case Op::Exp: return toWord(std::exp(value));
    case Op::Expm1: return toWord(std::expm1(value));
    case Op::Pow: return toWord(std::pow(value, other));
    case Op::Hypot: return toWord(std::hypot(value, other));
    case Op::Hypot3: return toWord(std::hypot(value, other, last));
    case Op::Fmod: return toWord(std::fmod(value, other));
    case Op::Remainder: return toWord(std::remainder(value, other));
    case Op::CopySign: return toWord(std::copysign(value, other));
    case Op::Cbrt: return toWord(std::cbrt(value));
    case Op::Tanh: return toWord(std::tanh(value));
    case Op::Floor: return toWord(std::floor(value));
    case Op::Ceiling: return toWord(std::ceil(value));
    case Op::Clamp: return toWord(model::clampFinite(value, other, last));
    case Op::ILogB: return std::bit_cast<std::uint64_t>(model::integerBinaryExponent(value));
    case Op::ScaleB: return toWord(model::scaleBinaryExponent(value, std::bit_cast<std::int64_t>(second)));
    case Op::FromInt: return toWord(model::roundInteger<Float>(std::bit_cast<std::int64_t>(first)));
    case Op::FromUInt32: return toWord(model::roundUInt32<Float>(static_cast<std::uint32_t>(first)));
    case Op::ToInt: return std::bit_cast<std::uint64_t>(model::truncateToInt(value));
    case Op::RoundToInt: return std::bit_cast<std::uint64_t>(model::roundToInt(value));
    case Op::ToUInt32: return model::truncateToUInt32(value);
    case Op::Widen: return toWord(model::widenFloat(fromWord<float>(first)));
    case Op::Narrow: return toWord(model::narrowFloat(fromWord<double>(first)));
    case Op::ObserveEnvironment: return model::observeFloatingEnvironment();
    case Op::RequireBinary32Environment:
        model::requireBinary32Environment();
        return 0;
    }
    throw model::ExecutionFailure("unknown native floating operation");
}

std::uint64_t evaluateFloating(FloatingBuiltin operation, bool binary32, std::uint64_t first,
                               std::uint64_t second, std::uint64_t third)
{
    if (binary32)
        return evaluate<float>(operation, first, second, third);
    return evaluate<double>(operation, first, second, third);
}

}

extern "C" std::int32_t pipelang_native_v19_floating(void* frame, std::int32_t operation, std::int32_t binary32,
                                                    std::uint64_t first, std::uint64_t second, std::uint64_t third,
                                                    std::uint64_t* output) noexcept
{
    return evaluateInInvocation(frame, evaluateFloating, static_cast<FloatingBuiltin>(operation), binary32 != 0,
                                 first, second, third, output);
}

extern "C" std::int32_t pipelang_native_v19_hosted_floating(void* construction, std::int32_t operation,
    std::int32_t binary32, std::uint64_t first, std::uint64_t second, std::uint64_t third,
    std::uint64_t* output) noexcept
{
    return withHostedConstruction(construction, [&](NativeHostedConstruction&) {
        *output = evaluateFloating(static_cast<FloatingBuiltin>(operation), binary32 != 0, first, second, third);
    });
}

}
