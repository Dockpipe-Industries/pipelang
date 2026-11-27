// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericFloatingFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericFloatingFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/FloatingQueries.pipe", R"pipe(module Numeric;
public model Class FloatingQueries {
    public pure float Widen(float32 value) => float.FromFloat32(value);
    public pure float32 Narrow(float value) => float32.FromFloat(value);
    public pure float32 RoundTrip(float32 value) => float32.FromFloat(float.FromFloat32(value));
    public pure float Hypot2(float first, float second) => float.Hypot(first, second);
    public pure float Hypot3(float first, float second, float third) => float.Hypot(first, second, third);
    public pure float NestedHypot(float first, float second, float third)
        => float.Hypot(float.Hypot(first, second), third);
    public pure float32 SingleHypot3(float32 first, float32 second, float32 third)
        => float32.Hypot(first, second, third);
    public pure float Remainder(float value, float divisor) => float.Remainder(value, divisor);
    public pure float Fmod(float value, float divisor) => float.Fmod(value, divisor);
    public pure float32 SingleRemainder(float32 value, float32 divisor) => float32.Remainder(value, divisor);
    public pure float Atan2(float vertical, float horizontal) => float.Atan2(vertical, horizontal);
    public pure float Sin(float angle) => float.Sin(angle);
    public pure float Cos(float angle) => float.Cos(angle);
    public pure float32 SingleAtan2(float32 vertical, float32 horizontal) => float32.Atan2(vertical, horizontal);
    public pure float32 SingleSin(float32 angle) => float32.Sin(angle);
    public pure float32 SingleCos(float32 angle) => float32.Cos(angle);

    public pure float FromInteger() => float.FromInt(1);
    public pure float32 SingleFromInteger() => float32.FromInt(1);
    public pure float FromUnsigned(uint32 value) => float.FromUInt32(value);
    public pure float32 SingleFromUnsigned(uint32 value) => float32.FromUInt32(value);
    public pure int IntegerFromDouble(float value) => int.FromFloat(value);
    public pure int IntegerFromSingle(float32 value) => int.FromFloat32(value);
    public pure int IntegerFromUnsigned(uint32 value) => int.FromUInt32(value);
    public pure uint32 UnsignedFromDouble(float value) => uint32.FromFloat(value);
    public pure uint32 UnsignedFromSingle(float32 value) => uint32.FromFloat32(value);
    public pure uint32 UnsignedFromInteger() => uint32.FromInt(1);
}
)pipe");
}

void verifyNativeNumericFloatingExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.FloatingQueries")
            methods.insert(item.at("method").get<std::string>());
    }
    require(methods == std::set<std::string>{"Widen", "Narrow", "RoundTrip", "Hypot2", "Hypot3", "NestedHypot",
        "SingleHypot3", "Remainder", "Fmod", "SingleRemainder", "Atan2", "Sin", "Cos", "SingleAtan2", "SingleSin", "SingleCos"},
        "floating numeric admission lost a safe operation or admitted an unqualified integer conversion");
}

std::string nativeNumericFloatingConsumer()
{
    return R"cpp(
template<class Query>
bool verifyFloatingProjection()
{
    static_assert(noexcept(Query::Widen(0.0f)));
    static_assert(noexcept(Query::Narrow(0.0)));
    static_assert(noexcept(Query::RoundTrip(0.0f)));
    static_assert(noexcept(Query::Hypot2(0.0, 0.0)));
    static_assert(noexcept(Query::Hypot3(0.0, 0.0, 0.0)));
    static_assert(noexcept(Query::NestedHypot(0.0, 0.0, 0.0)));
    static_assert(noexcept(Query::SingleHypot3(0.0f, 0.0f, 0.0f)));
    static_assert(noexcept(Query::Remainder(0.0, 1.0)));
    static_assert(noexcept(Query::Fmod(0.0, 1.0)));
    static_assert(noexcept(Query::SingleRemainder(0.0f, 1.0f)));
    static_assert(noexcept(Query::Atan2(0.0, 1.0)));
    static_assert(noexcept(Query::Sin(0.0)));
    static_assert(noexcept(Query::Cos(0.0)));
    static_assert(noexcept(Query::SingleAtan2(0.0f, 1.0f)));
    static_assert(noexcept(Query::SingleSin(0.0f)));
    static_assert(noexcept(Query::SingleCos(0.0f)));

    // Finite widening is exact; narrowing must retain every original bit.
    for (const auto bits : {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x807fffffu,
                           0x00800000u, 0x80800000u, 0x3f800001u, 0xbf800001u, 0x7f7fffffu, 0xff7fffffu}) {
        const float value = std::bit_cast<float>(bits);
        if (std::bit_cast<std::uint64_t>(Query::Widen(value)) !=
                std::bit_cast<std::uint64_t>(static_cast<double>(value)) ||
            std::bit_cast<std::uint32_t>(Query::RoundTrip(value)) != bits)
            return false;
    }
    constexpr double midpoint = 0x1.ffffffp127;
    const double belowMidpoint = std::nextafter(midpoint, 0.0);
    const double aboveMidpoint = std::nextafter(midpoint, std::numeric_limits<double>::infinity());
    for (const bool negative : {false, true}) {
        const auto sign = negative ? 0x80000000u : 0u;
        const double direction = negative ? -1.0 : 1.0;
        if (std::bit_cast<std::uint32_t>(Query::Narrow(direction * belowMidpoint)) != (sign | 0x7f7fffffu) ||
            std::bit_cast<std::uint32_t>(Query::Narrow(direction * midpoint)) != (sign | 0x7f800000u) ||
            std::bit_cast<std::uint32_t>(Query::Narrow(direction * aboveMidpoint)) != (sign | 0x7f800000u) ||
            std::bit_cast<std::uint32_t>(Query::Narrow(direction * 0x1p-150)) != sign ||
            std::bit_cast<std::uint32_t>(Query::Narrow(direction * std::nextafter(0x1p-150, 1.0))) != (sign | 1u))
            return false;
        const float infinity = std::bit_cast<float>(sign | 0x7f800000u);
        if (!std::isinf(Query::Widen(infinity)) || std::signbit(Query::Widen(infinity)) != negative ||
            std::bit_cast<std::uint32_t>(Query::Narrow(direction * std::numeric_limits<double>::infinity())) !=
                (sign | 0x7f800000u))
            return false;
    }
    // Conversion canonicalizes NaN payloads and preserves their sign, including
    // signalling inputs. These expected bits do not call the runtime conversion.
    for (const auto bits : {0x7fc12345u, 0xffc12345u, 0x7f812345u, 0xff812345u}) {
        const auto expected = (bits & 0x80000000u) ? UINT64_C(0xfff8000000000000) : UINT64_C(0x7ff8000000000000);
        if (std::bit_cast<std::uint64_t>(Query::Widen(std::bit_cast<float>(bits))) != expected)
            return false;
    }
    for (const auto bits : {UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff8123456789abc),
                           UINT64_C(0x7ff0123456789abc), UINT64_C(0xfff0123456789abc)}) {
        const auto expected = (bits >> 63) ? 0xffc00000u : 0x7fc00000u;
        if (std::bit_cast<std::uint32_t>(Query::Narrow(std::bit_cast<double>(bits))) != expected)
            return false;
    }

    for (const double scale : {0x1p1022, 0x1p-1022}) {
        const double first = scale;
        const double second = -scale * 0.5;
        const double third = scale * 0.25;
        const double expected = std::hypot(first, second, third);
        const double nested = std::hypot(std::hypot(first, second), third);
        if (Query::Hypot2(first, second) != std::hypot(first, second) ||
            Query::Hypot3(first, second, third) != expected ||
            Query::NestedHypot(first, second, third) != nested || !std::isfinite(expected) || expected <= 0.0 ||
            std::abs(nested / expected - 1.0) > 4.0 * std::numeric_limits<double>::epsilon())
            return false;
    }
    for (const float scale : {0x1p126f, 0x1p-126f}) {
        if (Query::SingleHypot3(scale, -scale * 0.5f, scale * 0.25f) !=
            std::hypot(scale, -scale * 0.5f, scale * 0.25f))
            return false;
    }
    // Nearest/even quotient selection differs from truncation at odd ties.
    for (const double value : {3.0, 5.0, 7.0, -3.0, -5.0, -7.0}) {
        if (Query::Remainder(value, 2.0) != std::remainder(value, 2.0) ||
            Query::Fmod(value, 2.0) != std::fmod(value, 2.0) ||
            Query::SingleRemainder(static_cast<float>(value), 2.0f) !=
                std::remainder(static_cast<float>(value), 2.0f))
            return false;
    }
    if (Query::Remainder(3.0, 2.0) != -1.0 || Query::Fmod(3.0, 2.0) != 1.0 ||
        Query::Remainder(5.0, 2.0) != 1.0 || Query::Remainder(7.0, 2.0) != -1.0)
        return false;
    for (const double angle : {-3.0, -1.0, -0.0, 0.0, 1.0, 3.0}) {
        const float single = static_cast<float>(angle);
        if (Query::Sin(angle) != std::sin(angle) || Query::Cos(angle) != std::cos(angle) ||
            Query::SingleSin(single) != std::sin(single) || Query::SingleCos(single) != std::cos(single))
            return false;
    }
    for (const double vertical : {-1.0, -0.0, 0.0, 1.0}) {
        for (const double horizontal : {-1.0, -0.0, 0.0, 1.0}) {
            if (std::bit_cast<std::uint64_t>(Query::Atan2(vertical, horizontal)) !=
                    std::bit_cast<std::uint64_t>(std::atan2(vertical, horizontal)) ||
                std::bit_cast<std::uint32_t>(Query::SingleAtan2(static_cast<float>(vertical), static_cast<float>(horizontal))) !=
                    std::bit_cast<std::uint32_t>(std::atan2(static_cast<float>(vertical), static_cast<float>(horizontal))))
                return false;
        }
    }
    return true;
}

bool verifyNumericFloating()
{
    return verifyFloatingProjection<numeric_fixture::native_numeric::Numeric::FloatingQueries>() &&
        verifyFloatingProjection<numeric_compiled_fixture::native_numeric::Numeric::FloatingQueries>();
}
)cpp";
}
