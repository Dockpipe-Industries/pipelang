// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeExponentFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"

void writeNativeExponentChecks(const std::filesystem::path& output)
{
    pipelang::tools::atomicWrite(output / "ExponentChecks.hpp", R"cpp(// Generated fixture from PipeLang's NativeExponentFixture.cpp.
inline float referenceNarrow(double value)
{
    float converted;
    // Independent instruction oracle: no out-of-range C++ floating conversion.
    __asm__ volatile("cvtsd2ss %1, %0" : "=x"(converted) : "x"(value));
    const auto bits = std::bit_cast<std::uint32_t>(converted);
    if ((bits & 0x7fffffffU) > 0x7f800000U)
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7fc00000U);
    return converted;
}

template<class Float>
std::int64_t referenceExponent(Float value)
{
    volatile Float input = value;
    const int exponent = std::ilogb(input);
    using Bits = std::conditional_t<sizeof(Float) == 4, std::uint32_t, std::uint64_t>;
    const Bits bits = std::bit_cast<Bits>(value);
    const Bits sign = Bits{1} << (sizeof(Bits) * 8 - 1);
    const Bits infinity = sizeof(Float) == 4 ? Bits(0x7f800000U) : Bits(UINT64_C(0x7ff0000000000000));
    const Bits magnitude = bits & ~sign;
    if (magnitude == 0 || magnitude > infinity)
        return INT64_C(-2147483648);
    if (magnitude == infinity)
        return INT64_C(2147483647);
    return exponent;
}

template<class Operation, class Reference>
bool sameExponentFloating(Operation operation, Reference reference)
{
    // Every case observes newly raised flags; earlier invalid/overflow cases
    // must not hide a later operation that failed to raise its required flag.
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    return sameFloating(operation, reference);
}

template<class Query>
bool verifyExponentOperations()
{
    static_assert(noexcept(Query::Narrow(0.0)));
    static_assert(noexcept(Query::WideExponent(0.0)));
    static_assert(noexcept(Query::WideScale(0.0, 0)));
    const double wideValues[] = {
        0.0, -0.0, 0x1p-1074, -0x1p-1074, 0x1p-1022, 0.5, 1.0, -3.0,
        0x1.fffffffffffffp1023,
        std::bit_cast<double>(UINT64_C(0x7ff0000000000000)),
        std::bit_cast<double>(UINT64_C(0xfff0000000000000)),
        std::bit_cast<double>(UINT64_C(0x7ff8123456789abc)),
        std::bit_cast<double>(UINT64_C(0xfff8123456789abc)),
        std::bit_cast<double>(UINT64_C(0x7ff0123456789abc)),
        std::bit_cast<double>(UINT64_C(0xfff0123456789abc))
    };
    const std::int64_t exponents[] = {
        INT64_MIN, INT64_C(-2147483649), INT64_C(-2147483648), -1075, -149, -1, 0, 1,
        128, 1024, INT64_C(2147483647), INT64_C(2147483648), INT64_MAX
    };
    for (const double value : wideValues) {
        if (!sameExponentFloating([&] { return Query::WideExponent(value); }, [&] { return referenceExponent(value); }))
            return false;
        for (const std::int64_t exponent : exponents) {
            int nativeExponent;
            if (exponent < INT32_MIN) {
                nativeExponent = INT32_MIN;
            } else if (exponent > INT32_MAX) {
                nativeExponent = INT32_MAX;
            } else {
                nativeExponent = static_cast<int>(exponent);
            }
            if (!sameExponentFloating([&] { return Query::WideScale(value, exponent); }, [&] {
                    volatile double input = value;
                    volatile double result = std::ldexp(input, nativeExponent);
                    return result;
                }))
                return false;
        }
        for (const double other : {0.0, 2.0, 0x1p-1074, std::numeric_limits<double>::infinity()}) {
            if (!sameExponentFloating([&] { return Query::WideRemainder(value, other); }, [&] {
                    volatile double first = value;
                    volatile double second = other;
                    return std::fmod(first, second);
                }))
                return false;
        }
    }
    for (const std::uint32_t bits : {0U, 0x80000000U, 1U, 0x80000001U, 0x00800000U, 0x3f000000U,
                                    0x3f800000U, 0xc0400000U, 0x7f7fffffU, 0x7f800000U, 0xff800000U,
                                    0x7fc12345U, 0xffc12345U, 0x7f812345U, 0xff812345U}) {
        const float value = std::bit_cast<float>(bits);
        if (!sameExponentFloating([&] { return Query::SingleExponent(value); }, [&] { return referenceExponent(value); }))
            return false;
        for (const std::int64_t exponent : exponents) {
            int nativeExponent;
            if (exponent < INT32_MIN) {
                nativeExponent = INT32_MIN;
            } else if (exponent > INT32_MAX) {
                nativeExponent = INT32_MAX;
            } else {
                nativeExponent = static_cast<int>(exponent);
            }
            if (!sameExponentFloating([&] { return Query::SingleScale(value, exponent); }, [&] {
                    volatile float input = value;
                    volatile float result = std::ldexp(input, nativeExponent);
                    return result;
                }))
                return false;
        }
        if (!sameExponentFloating([&] { return Query::SingleRemainder(value, 2.0f); }, [&] {
                volatile float first = value;
                volatile float second = 2.0f;
                volatile float result = std::fmod(first, second);
                return result;
            }))
            return false;
    }
    const double narrowValues[] = {
        0.0, -0.0, 0x1p-1074, -0x1p-1074, 0x1p-151, -0x1p-151, 0x1p-150, -0x1p-150,
        0x1p-149, -0x1p-149, 0x1.fffffep-127, 0x1p-126, 0x1.000001p0, -0x1.000001p0,
        0x1.000003p0, -0x1.000003p0, 0x1.fffffep127, -0x1.fffffep127,
        0x1.fffffe8p127, -0x1.fffffe8p127, 0x1.ffffffp127, -0x1.ffffffp127,
        0x1p128, -0x1p128, 0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023,
        std::bit_cast<double>(UINT64_C(0x7ff0000000000000)),
        std::bit_cast<double>(UINT64_C(0xfff0000000000000)),
        std::bit_cast<double>(UINT64_C(0x7ff8123456789abc)),
        std::bit_cast<double>(UINT64_C(0xfff8123456789abc)),
        std::bit_cast<double>(UINT64_C(0x7ff0123456789abc)),
        std::bit_cast<double>(UINT64_C(0xfff0123456789abc))
    };
    for (const double value : narrowValues) {
        if (!sameExponentFloating([&] { return Query::Narrow(value); }, [&] { return referenceNarrow(value); }))
            return false;
    }
    if (!sameExponentFloating([] { return Query::LiteralScale(); }, [] {
            volatile double value = 1.0;
            volatile double result = std::ldexp(value, -1075);
            return result;
        }) || !sameExponentFloating([] { return Query::LiteralNarrow(); }, [] {
            volatile double value = 1.000000059604644775390625;
            return referenceNarrow(value);
        }) || !sameExponentFloating([] { return Query::LiteralRemainder(); }, [] {
            volatile double value = 5.5;
            volatile double divisor = 2.0;
            return std::fmod(value, divisor);
        }))
        return false;
    // Independent known exponents and source integer-range/negation composition.
    return Query::WideExponent(0.5) == -1 && Query::WideExponent(1.0) == 0 &&
        Query::WideExponent(3.0) == 1 && Query::WideExponent(0x1.fffffffffffffp1023) == 1023 &&
        Query::SingleExponent(0x1p-126f) == -126 && Query::ExponentStep(1.0) == 10 &&
        Query::ExponentStep(0.5) == 11;
}
)cpp");
}
