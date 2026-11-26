// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"
#include "NumericRecordAbi.h"

#include <bit>
#include <cfenv>
#include <cstdint>
#include <immintrin.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>

using Reference = reference::native_numeric::NativeProof::NumericExport::Queries;
namespace Authored = reference::source::NativeProof::NumericExport;
extern "C" float pipe_narrow(double);
extern "C" double pipe_widen(float);
extern "C" float pipe_add(float, float);
extern "C" float pipe_divide(float, float);
extern "C" float pipe_literal_divide();
extern "C" float pipe_qualified_add(float, float);
extern "C" float pipe_single(std::int64_t);
extern "C" double pipe_wide(std::int64_t);
extern "C" std::int64_t pipe_signed_identity(std::int64_t);
extern "C" std::uint32_t pipe_unsigned_identity(std::uint32_t);
extern "C" double pipe_choose(double, double);

namespace {

class FloatingEnvironment {
public:
    FloatingEnvironment()
    {
        if (std::fegetenv(&environment_) != 0)
            throw std::runtime_error("unable to save the floating environment");
    }

    ~FloatingEnvironment() { restore(); }

    void restore() const
    {
        std::fesetenv(&environment_);
        _mm_setcsr(control_);
    }

private:
    std::fenv_t environment_;
    unsigned control_ = _mm_getcsr();
};

template<class T>
auto bits(T value)
{
    using Word = std::conditional_t<std::is_same_v<T, float>, std::uint32_t, std::uint64_t>;
    return std::bit_cast<Word>(value);
}

template<class Oracle, class Native>
void compareObservation(const char* method, Oracle reference, Native direct)
{
    FloatingEnvironment original;
    const auto expected = reference();
    const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
    const auto rounding = std::fegetround();
    const auto control = _mm_getcsr();
    original.restore();

    const auto actual = direct();
    if (actual != expected || std::fetestexcept(FE_ALL_EXCEPT) != flags ||
        std::fegetround() != rounding || _mm_getcsr() != control) {
        throw std::runtime_error(std::string(method) + " changed the reference value or floating environment");
    }
}

template<class Oracle, class Native>
void compare(const char* method, Oracle reference, Native direct)
{
    compareObservation(method, [&] { return bits(reference()); }, [&] { return bits(direct()); });
}

template<class PointType>
auto recordBits(const PointType& point)
{
    return std::tuple{bits(point.wide), bits(point.narrow), point.code, point.tag};
}

void checkRecords()
{
    const auto wideTiny = std::numeric_limits<double>::denorm_min();
    const auto narrowTiny = std::numeric_limits<float>::denorm_min();
    const double shift = 0x1p-53;
    const Point inputs[] = {
        {1.0, 1.0f, UINT32_MAX, INT64_MIN},
        {-0.0, narrowTiny, 0, INT64_MAX},
        {wideTiny, -narrowTiny, 1, -1},
        {std::numeric_limits<double>::quiet_NaN(), -0.0f, UINT32_MAX, 0}
    };
    for (const auto& input : inputs) {
        const Authored::Point authored{input.wide, input.narrow, input.code, input.tag};
        compareObservation("Numeric records", [&] {
            const auto adjusted = Reference::AdjustPoint(authored, shift);
            const auto nested = Reference::NestPoint(authored, shift);
            const auto inPlace = Reference::AdjustPoint(authored, shift);
            return std::tuple{recordBits(adjusted), recordBits(nested.point), bits(nested.ratio),
                              Reference::PointTag(authored), recordBits(inPlace)};
        }, [&] {
            Point adjusted{};
            Pair nested{};
            std::int64_t tag{};
            pipe_check_record_abi(&input, shift, &adjusted, &nested, &tag);

            Point inPlace = input;
            pipe_adjust_point(&inPlace, shift, &inPlace);
            return std::tuple{recordBits(adjusted), recordBits(nested.point), bits(nested.ratio), tag, recordBits(inPlace)};
        });
    }
}

void checkConversions()
{
    const auto tiny = std::numeric_limits<float>::denorm_min();
    const auto tinyWide = std::numeric_limits<double>::denorm_min();
    for (double value : {1.0 + 0x1p-24, 0x1.ffffffp127, -0x1.ffffffp127, tinyWide, -tinyWide,
            std::bit_cast<double>(UINT64_C(0x7ff0123456789abc)),
            std::bit_cast<double>(UINT64_C(0xfff8123456789abc))}) {
        compare("Narrow", [value] { return Reference::Narrow(value); }, [value] { return pipe_narrow(value); });
    }
    for (float value : {tiny, -tiny, -0.0f, 1.0f, std::bit_cast<float>(UINT32_C(0x7f812345)),
            std::bit_cast<float>(UINT32_C(0xffc12345))}) {
        compare("Widen", [value] { return Reference::Widen(value); }, [value] { return pipe_widen(value); });
    }
    for (std::int64_t value : {INT64_C(16777217), -INT64_C(16777217), INT64_C(9007199254740993),
            -INT64_C(9007199254740993), INT64_MIN, INT64_MAX}) {
        compare("SingleFromInteger", [value] { return Reference::SingleFromInteger(value); },
                [value] { return pipe_single(value); });
        compare("WideFromInteger", [value] { return Reference::WideFromInteger(value); },
                [value] { return pipe_wide(value); });
    }
}

void checkArithmetic()
{
    const auto tiny = std::numeric_limits<float>::denorm_min();
    compare("Add", [] { return Reference::Add(1.0f, 0x1p-24f); },
            [] { return pipe_add(1.0f, 0x1p-24f); });
    compare("Add subnormal", [tiny] { return Reference::Add(tiny, tiny); },
            [tiny] { return pipe_add(tiny, tiny); });
    compare("Divide", [] { return Reference::Divide(1.0f, 3.0f); },
            [] { return pipe_divide(1.0f, 3.0f); });
    compare("LiteralDivide", [] { return Reference::LiteralDivide(); }, [] { return pipe_literal_divide(); });
    compare("ThroughHelper", [] { return Reference::ThroughHelper(1.0f, 0x1p-24f); },
            [] { return pipe_qualified_add(1.0f, 0x1p-24f); });
    compare("Choose", [] { return Reference::Choose(1.0, 2.0); }, [] { return pipe_choose(1.0, 2.0); });
    compare("Choose signed zero", [] { return Reference::Choose(0.0, -0.0); }, [] { return pipe_choose(0.0, -0.0); });
}

}

int main()
{
    try {
        const FloatingEnvironment original;
        if (pipe_signed_identity(INT64_MIN) != Reference::SignedIdentity(INT64_MIN) ||
            pipe_unsigned_identity(UINT32_MAX) != Reference::UnsignedIdentity(UINT32_MAX)) {
            throw std::runtime_error("integer C ABI changed an input endpoint");
        }
        for (const int rounding : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
            if (std::fesetround(rounding) != 0)
                throw std::runtime_error("rounding mode unavailable");
            const auto initialControl = _mm_getcsr();
            for (const unsigned mxcsrRounding : {0U, 0x2000U, 0x4000U, 0x6000U}) {
                for (const unsigned denormals : {0U, 0x40U, 0x8000U, 0x8040U}) {
                    _mm_setcsr((initialControl & ~0xe040U) | mxcsrRounding | denormals);
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::feraiseexcept(FE_DIVBYZERO);
                    checkConversions();
                    checkArithmetic();
                    checkRecords();
                }
            }
        }
        std::cout << "Direct C ABI numeric exports preserved active values, controls, and exception flags.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
