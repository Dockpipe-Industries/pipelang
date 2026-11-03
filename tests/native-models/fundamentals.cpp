// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fundamentals.hpp"
#include "NullableSamplerProbe.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <bit>
#include <cassert>
#include <cfenv>
#include <iostream>
#include <type_traits>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

using namespace pipelang_models;
using pipelang::model::Optional;
using pipelang::model::Result;

void integerRemainderSemantics() {
    Fundamentals model;
    assert(model.MinimumRemainder() == 0);
    assert(model.RemainderPrecedence() == 25);
    assert(model.RemainderLeftAssociative() == 1);
    assert(model.RemainderAfterDivision() == 2);
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto left : {minimum, minimum + 1, std::int64_t(-7), std::int64_t(-1),
                            std::int64_t(0), std::int64_t(1), std::int64_t(7), maximum}) {
        for (const auto right : {minimum, std::int64_t(-3), std::int64_t(-1),
                                 std::int64_t(1), std::int64_t(3), maximum}) {
            const auto actual = model.Remainder(left, right);
            if (left == minimum && right == -1) {
                assert(actual == 0);
            } else {
                const auto quotient = left / right;
                // Independent quotient identity; its product is in range for
                // truncated division, including either signed64 boundary.
                assert(actual == left - quotient * right);
            }
            assert(actual == 0 || (actual < 0) == (left < 0));
        }
    }
    for (const bool constant : {false, true}) {
        bool refused = false;
        try {
            (void)(constant ? model.ConstantRemainderFailure() : model.Remainder(7, 0));
        } catch (const pipelang::model::ExecutionFailure& error) {
            refused = true;
            assert(std::string(error.what()) == "integer remainder by zero");
        }
        assert(refused);
    }
    const auto before = model.snapshot();
    const auto failed = model.RemainderRollback(0);
    assert(failed.is_failure() && failed.error() == "integer remainder by zero");
    assert(model.snapshot() == before);
    const auto succeeded = model.RemainderRollback(-5);
    assert(succeeded.is_success() && succeeded.value() == 4);
    assert(model.snapshot().count == 99);
}

void numericSemantics() {

    Fundamentals model;
    assert(std::bit_cast<std::uint64_t>(model.Decimal()) == UINT64_C(0x3fb999999999999a));
    assert(std::bit_cast<std::uint64_t>(model.NegativeZero()) == UINT64_C(0x8000000000000000));
    assert(model.Scientific() == 1250.0);
    assert(std::bit_cast<std::uint64_t>(model.Smallest()) == 1);
    assert(model.Largest() == std::numeric_limits<double>::max());
    assert(model.Absolute(-3.5) == 3.5);
    assert(!std::signbit(model.Absolute(-0.0)));
    assert(model.Root(4.0) == 2.0);
    assert(std::isnan(model.Root(-1.0)));
    assert(model.Logarithm(1.0) == 0.0);
    assert(model.Logarithm(0.0) == -std::numeric_limits<double>::infinity());
    assert(std::isnan(model.Logarithm(-1.0)));
    assert(model.Composed(1.5) == 3.5);
    assert(model.Subtract(4.5, 1.25) == 3.25);
    assert(model.Divide(5.0, 2.0) == 2.5);
    assert(model.Scoped(true).value() == 21);
    assert(model.Scoped(false).value() == 13);
    assert(model.ReturnFromBlock().value() == 41);
    assert(model.SafeArm(true) == 1.25);
    assert(!model.SafeAnd());
    assert(model.SafeOr());

    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double tiny = std::numeric_limits<double>::denorm_min();
    assert(model.Finite(tiny));
    assert(!model.Finite(infinity));
    assert(!model.Finite(-infinity));
    assert(!model.Finite(nan));
    assert(model.Multiply(tiny, 1.0) == tiny);
    assert(std::isinf(model.Add(infinity, 1.0)));
    assert(std::isnan(model.Subtract(infinity, infinity)));
    assert(std::isnan(model.Multiply(infinity, 0.0)));
    assert(std::isnan(model.Divide(0.0, 0.0)));
    assert(model.Divide(1.0, -0.0) == -infinity);
    assert(!model.Less(nan, 1.0));
    assert(!model.LessEqual(nan, 1.0));
    assert(!model.Greater(nan, 1.0));
    assert(!model.GreaterEqual(nan, 1.0));
    assert(!model.Equal(nan, nan));
    assert(model.Different(nan, nan));
    assert(model.Equal(0.0, -0.0));
    assert(std::signbit(model.FirstMinimum(-0.0, 0.0)));
    assert(!std::signbit(model.FirstMinimum(0.0, -0.0)));
    assert(std::isnan(model.FirstMinimum(nan, 1.0)));
    assert(model.FirstMinimum(1.0, nan) == 1.0);
    assert(model.Store(infinity).is_success());
    assert(model.snapshot().stored == infinity);
    assert(model.Store(-0.0).is_success());
    assert(std::signbit(model.snapshot().stored));
}

void nullableSyntaxSemantics() {
    Fundamentals model;
    const auto present = model.NullableSample(0);
    const auto absent = model.NullableSample(1);
    const auto failed = model.NullableSample(2);
    const auto cancelled = model.NullableSample(3);
    assert(present.is_success() && present.value().has_value() && present.value().value() == 3.5f);
    assert(absent.is_success() && !absent.value().has_value());
    assert(failed.is_failure() && failed.error() == "domain failure");
    assert(cancelled.is_cancelled() && cancelled.error() == "cancelled exactly");
    assert(model.NullableChoice(true).value() == 7);
    assert(!model.NullableChoice(false).has_value());
    assert(!model.NullableReverseChoice(true).has_value());
    assert(model.NullableReverseChoice(false).value() == 9);
    assert(!model.NullableBothAbsent(true).has_value());
    assert(!model.NullableBothAbsent(false).has_value());
    const auto beforeCondition = model.snapshot().count;
    for (const bool choose : {true, false}) {
        const auto counted = model.NullableCountedAbsent(choose);
        assert(counted.is_success() && !counted.value().has_value());
    }
    assert(model.snapshot().count == beforeCondition + 2);
    assert(!model.NullableOuterAbsent().has_value());
    const auto innerAbsent = model.NullableInnerAbsent();
    assert(innerAbsent.has_value() && !innerAbsent.value().has_value());
    assert(model.NullableNestedValue().value().value() == 11);
    assert(model.NullableGeneric().value() == 17);
    assert(model.NullableStorage());
    assert(model.NullableIdentity(Optional<std::int64_t>::Some(23)).value() == 23);
    assert(!model.NullableIdentity(Optional<std::int64_t>::None()).has_value());
    bool refused = false;
    try {
        (void)model.NullableCheckedRead(Optional<std::int64_t>::None());
    } catch (const pipelang::model::ExecutionFailure &error) {
        refused = std::string(error.what()) == "Optional.Value requires Some";
    }
    assert(refused);
    auto sampler = std::make_shared<nullable_tests::NullableSamplerProbe>();
    NullableCallbacks callbacks(sampler);
    for (int mode = 0; mode < 4; ++mode) {
        sampler->mode = mode;
        const auto actual = callbacks.Echo();
        const auto expected = model.NullableSample(mode);
        assert(actual == expected);
    }
}

void ownedOptionalSemantics() {
    Fundamentals model;
    assert(model.DefaultNone());
    assert(!model.Absent().has_value());
    assert(model.Present(2.5).value() == 2.5);
    Label source{"original"};
    assert(model.SetLabel(source).is_success());
    source.text = "host edit";
    assert(model.ReadLabel().text == "original");
    auto detached = model.snapshot();
    detached.label = Optional<Label>::Some(Label{"snapshot edit"});
    assert(model.ReadLabel().text == "original");
    const auto before = model.snapshot();
    assert(model.MissingRollsBack().is_failure());
    assert(model.snapshot() == before);

    Envelope valid{Optional<Result<Label, std::string>>::Some(
        Result<Label, std::string>::Success(Label{"nested"}))};
    assert(model.SetEnvelope(valid).is_success());
    valid.payload = Optional<Result<Label, std::string>>::None();
    assert(model.snapshot().envelope.payload.value().value().text == "nested");
    const auto saved = model.snapshot();
    const std::string malformed("\xff", 1);
    for (bool failure : {false, true}) {
        auto outcome = failure
            ? Result<Label, std::string>::Failure(malformed)
            : Result<Label, std::string>::Success(Label{malformed});
        Envelope invalid{Optional<Result<Label, std::string>>::Some(outcome)};
        const auto refused = model.SetEnvelope(invalid);
        assert(refused.is_failure());
        assert(refused.error() == "string contains invalid UTF-8");
        assert(model.snapshot() == saved);
    }
}

void binary32Semantics() {
    Binary32 model;
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    static_assert(std::is_same_v<decltype(model.Decimal()), float>);
    static_assert(std::is_same_v<decltype(model.snapshot().stored), float>);
    assert(bits(model.Decimal()) == UINT32_C(0x3dcccccd));
    assert(bits(model.Generic()) == UINT32_C(0x3dcccccd));
    assert(bits(model.Exchange(-0.0f).value()) == 0);
    assert(bits(model.CompareExchange(1.0f, 0.0f).value()) == UINT32_C(0x80000000));
    assert(bits(model.Exchange(2.0f).value()) == UINT32_C(0x80000000));
    assert(bits(model.Tie()) == UINT32_C(0x3f800000));
    assert(bits(model.Smallest()) == 1);
    assert(model.Largest() == std::numeric_limits<float>::max());
    assert(bits(model.NegativeZero()) == UINT32_C(0x80000000));
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::bit_cast<float>(UINT32_C(0x7fc01234));
    const float tiny = std::numeric_limits<float>::denorm_min();
    assert(model.Finite(tiny));
    assert(!model.Finite(infinity));
    assert(!model.Finite(nan));
    assert(bits(model.Multiply(tiny, 1.0f)) == 1);
    assert(model.Divide(1.0f, -0.0f) == -infinity);
    assert(std::isnan(model.Divide(0.0f, 0.0f)));
    assert(std::isnan(model.Subtract(infinity, infinity)));
    assert(std::isnan(model.Multiply(infinity, 0.0f)));
    assert(std::isinf(model.Add(infinity, 1.0f)));
    assert(!model.Less(nan, 1.0f));
    assert(!model.LessEqual(nan, 1.0f));
    assert(!model.Greater(nan, 1.0f));
    assert(!model.GreaterEqual(nan, 1.0f));
    assert(bits(model.FirstMinimum(-0.0f, 0.0f)) == UINT32_C(0x80000000));
    assert(bits(model.FirstMinimum(0.0f, -0.0f)) == 0);
    assert(bits(model.FirstMinimum(nan, 1.0f)) == bits(nan));
    assert(model.FirstMinimum(1.0f, nan) == 1.0f);
    // This product rounds to one before addition. Fusing would retain a tiny
    // nonzero result, changing ordinary source evaluation.
    assert(bits(model.MultiplyAdd(0x1.000002p0f, 0x1.fffffcp-1f, -1.0f)) == 0);
    assert(!model.Equal(nan, nan));
    assert(model.Different(nan, nan));
    assert(bits(model.Negative(0.0f)) == UINT32_C(0x80000000));
    assert(bits(model.Absolute(-0.0f)) == 0);
    assert(model.Root(4.0f) == 2.0f);
    assert(std::isnan(model.Root(-1.0f)));
    assert(model.Logarithm(1.0f) == 0.0f);
    assert(model.Logarithm(0.0f) == -infinity);
    assert(model.Store(nan).is_success());
    assert(bits(model.snapshot().stored) == bits(nan));
    assert(bits(model.snapshot().values.at(0)) == bits(nan));
    assert(model.Store(-0.0f).is_success());
    assert(model.Rollback().is_failure());
    assert(bits(model.snapshot().stored) == UINT32_C(0x80000000));
    assert(model.snapshot().values.count() == 2);

    std::uint32_t random = 1729;
    const auto next = [&]() {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        return std::bit_cast<float>(UINT32_C(0x3f000000) | (random & UINT32_C(0x007fffff)));
    };
    for (int index = 0; index < 100000; ++index) {
        const float friction = next();
        const float impulse = next();
        const float speed = next();
        const float expected = std::min(1.0f, friction * impulse / speed);
        assert(bits(model.Contact(friction, impulse, speed)) == bits(expected));
        assert(bits(model.Add(friction, impulse)) == bits(friction + impulse));
        assert(bits(model.Subtract(friction, impulse)) == bits(friction - impulse));
        assert(bits(model.Multiply(friction, impulse)) == bits(friction * impulse));
        assert(bits(model.Divide(friction, speed)) == bits(friction / speed));
    }
    const int previous = std::fegetround();
    assert(std::fesetround(FE_DOWNWARD) == 0);
    bool refused = false;
    try {
        (void)model.Decimal();
    } catch (const pipelang::model::ExecutionFailure&) {
        refused = true;
    }
    assert(std::fesetround(previous) == 0);
    assert(refused);
#if defined(__SSE__) || defined(_M_X64)
    const unsigned previousControl = _mm_getcsr();
    for (unsigned mode : {0x8000U, 0x0040U, 0x2000U, 0x4000U, 0x6000U}) {
        _mm_setcsr(previousControl | mode);
        refused = false;
        try {
            (void)model.Decimal();
        } catch (const pipelang::model::ExecutionFailure&) {
            refused = true;
        }
        _mm_setcsr(previousControl);
        assert(refused);
    }
#endif
    std::cout << "binary32: 100000 native contact/arithmetic bit comparisons, literals, IEEE, rollback and mode refusals passed\n";
}

template<class Float>
void expectSameNumeric(Float actual, Float expected) {
    if (std::isnan(expected)) {
        assert(std::isnan(actual));
    } else {
        assert(actual == expected);
        if (expected == Float(0))
            assert(std::signbit(actual) == std::signbit(expected));
    }
}

template<class Float, class Model>
void typedMathSemantics(Model& model) {
    const Float infinity = std::numeric_limits<Float>::infinity();
    const Float nan = std::numeric_limits<Float>::quiet_NaN();
    const Float maximum = std::numeric_limits<Float>::max();
    const Float tiny = std::numeric_limits<Float>::denorm_min();
    for (const Float value : {Float(-0.0), Float(0.0), Float(-1), Float(1), Float(-2), Float(2),
                              Float(-0.5), Float(0.5), tiny, -tiny, maximum, -maximum,
                              infinity, -infinity, nan}) {
        expectSameNumeric(model.Sin(value), std::sin(value));
        expectSameNumeric(model.Cos(value), std::cos(value));
        expectSameNumeric(model.Tan(value), std::tan(value));
        expectSameNumeric(model.Asin(value), std::asin(value));
        expectSameNumeric(model.Acos(value), std::acos(value));
        expectSameNumeric(model.Atan(value), std::atan(value));
        expectSameNumeric(model.Exp(value), std::exp(value));
        expectSameNumeric(model.Expm1(value), std::expm1(value));
        expectSameNumeric(model.Cbrt(value), std::cbrt(value));
        expectSameNumeric(model.Tanh(value), std::tanh(value));
        expectSameNumeric(model.Floor(value), std::floor(value));
        expectSameNumeric(model.Ceiling(value), std::ceil(value));
        for (const Float other : {Float(-0.0), Float(0.0), Float(-1), Float(1), Float(2),
                                  Float(0.5), tiny, maximum, infinity, -infinity, nan}) {
            expectSameNumeric(model.Atan2(value, other), std::atan2(value, other));
            expectSameNumeric(model.Pow(value, other), std::pow(value, other));
            expectSameNumeric(model.Hypot(value, other), std::hypot(value, other));
            expectSameNumeric(model.Hypot3(value, other, tiny), std::hypot(value, other, tiny));
            expectSameNumeric(model.Hypot3(value, other, infinity), std::hypot(value, other, infinity));

            expectSameNumeric(model.Fmod(value, other), std::fmod(value, other));
            expectSameNumeric(model.Remainder(value, other), std::remainder(value, other));
            expectSameNumeric(model.CopySign(value, other), std::copysign(value, other));
        }
    }
    // fmod truncates its quotient; IEEE remainder chooses the nearest/even one.
    assert(model.Hypot3(Float(0), Float(0), Float(7)) == Float(7));
    expectSameNumeric(model.Hypot3(maximum / Float(2), maximum / Float(2), maximum / Float(2)),
                      std::hypot(maximum / Float(2), maximum / Float(2), maximum / Float(2)));
    assert(model.Fmod(Float(3), Float(2)) == Float(1));

    assert(model.Remainder(Float(3), Float(2)) == Float(-1));
    assert(std::isinf(model.Hypot(infinity, nan)));
    assert(std::signbit(model.Expm1(Float(-0.0))));
    assert(std::signbit(model.CopySign(Float(0), Float(-1))));
    assert(model.Cbrt(Float(-8)) == Float(-2));
    assert(model.Floor(Float(-1.25)) == Float(-2));
    assert(model.Ceiling(Float(-1.25)) == Float(-1));
    const auto domain = model.PreserveDomain(Float(2));
    assert(domain.is_success() && std::isnan(domain.value()));
    assert(model.snapshot().count == 7);
}

template<class Float, class Model>
void integerConversionSemantics(Model& model) {
    for (const std::int64_t value : {std::numeric_limits<std::int64_t>::min(),
                                     INT64_C(-9007199254740993), INT64_C(-16777217), INT64_C(-1),
                                     INT64_C(0), INT64_C(1), INT64_C(16777217),
                                     INT64_C(9007199254740993), std::numeric_limits<std::int64_t>::max()}) {
        volatile Float expected = static_cast<Float>(value);
        expectSameNumeric(model.FromInt(value), Float(expected));
    }
    const Float boundary = Float(0x1p63);
    const Float belowBoundary = std::nextafter(boundary, Float(0));
    for (const Float value : {Float(-0.0), Float(0), Float(-1.75), Float(1.75),
                              -boundary, belowBoundary, std::numeric_limits<Float>::denorm_min()})
        assert(model.ToInt(value) == static_cast<std::int64_t>(value));
    for (const Float value : {boundary, std::nextafter(-boundary, -std::numeric_limits<Float>::infinity()),
                              std::numeric_limits<Float>::max(), std::numeric_limits<Float>::infinity(),
                              -std::numeric_limits<Float>::infinity(), std::numeric_limits<Float>::quiet_NaN()}) {
        bool refused = false;
        try {
            (void)model.ToInt(value);
        } catch (const pipelang::model::ExecutionFailure& error) {
            refused = true;
            assert(std::string(error.what()) == "floating-to-int conversion requires a finite value in signed64 range");
        }
        assert(refused);
        const auto before = model.snapshot();
        const auto failed = model.ConvertRollback(value);
        assert(failed.is_failure());
        assert(model.snapshot() == before);
    }
    const auto recovered = model.ConvertRollback(Float(42.75));
    assert(recovered.is_success() && recovered.value() == 42);
    assert(model.snapshot().count == 99);
}

void explicitFloatingConversions() {
    Math64 wide;
    Math32 narrow;
    const float infinity = std::numeric_limits<float>::infinity();
    for (const float value : {-0.0f, 0.0f, -1.0f, 1.0f, std::numeric_limits<float>::denorm_min(),
                              -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
                              infinity, -infinity, std::numeric_limits<float>::quiet_NaN()})
        expectSameNumeric(wide.Widen(value), static_cast<double>(value));

    // Every finite test in this loop is in the float range; the native cast is
    // an independent nominal-width oracle without out-of-range conversion UB.
    const double maximum = std::numeric_limits<float>::max();
    const double tiny = std::numeric_limits<float>::denorm_min();
    for (const double value : {-0.0, 0.0, -1.0, 1.0, 0.1, -0.1, tiny, -tiny,
                               tiny * 0.5, -tiny * 0.5, tiny * 0.75, -tiny * 0.75,
                               std::numeric_limits<double>::denorm_min(), maximum, -maximum,
                               1.000000059604644775390625, 1.000000178813934326171875}) {
        volatile float expected = static_cast<float>(value);
        expectSameNumeric(narrow.Narrow(value), float(expected));
    }
    std::uint64_t random = UINT64_C(0x91e10da5c79e7b1d);
    for (int index = 0; index < 4096; ++index) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        const double value = std::bit_cast<double>(random);
        if (std::isfinite(value) && std::abs(value) <= maximum) {
            volatile float expected = static_cast<float>(value);
            expectSameNumeric(narrow.Narrow(value), float(expected));
        }
        const float single = std::bit_cast<float>(static_cast<std::uint32_t>(random));
        expectSameNumeric(wide.Widen(single), static_cast<double>(single));
    }
    constexpr double midpoint = 0x1.ffffffp127;

    for (const double sign : {-1.0, 1.0}) {
        expectSameNumeric(narrow.Narrow(sign * std::nextafter(midpoint, 0.0)),
                          std::copysign(std::numeric_limits<float>::max(), float(sign)));
        expectSameNumeric(narrow.Narrow(sign * midpoint), std::copysign(infinity, float(sign)));
        expectSameNumeric(narrow.Narrow(sign * std::nextafter(midpoint, std::numeric_limits<double>::infinity())),
                          std::copysign(infinity, float(sign)));
        expectSameNumeric(narrow.Narrow(sign * std::numeric_limits<double>::max()),
                          std::copysign(infinity, float(sign)));
        expectSameNumeric(narrow.Narrow(sign * std::numeric_limits<double>::infinity()),
                          std::copysign(infinity, float(sign)));
    }
    assert(std::isnan(narrow.Narrow(std::numeric_limits<double>::quiet_NaN())));
    assert(std::signbit(narrow.Narrow(-std::numeric_limits<double>::denorm_min())));
    assert(narrow.FromInt(INT64_C(16777217)) == 16777216.0f);
    assert(wide.FromInt(INT64_C(9007199254740993)) == 9007199254740992.0);
    assert(std::bit_cast<std::uint32_t>(narrow.Narrow(tiny * 0.5)) == 0);
    assert(std::bit_cast<std::uint32_t>(narrow.Narrow(tiny * 0.75)) == 1);
}

void extendedNumericSemantics() {
    Math64 wide;
    Math32 narrow;
    typedMathSemantics<double>(wide);
    typedMathSemantics<float>(narrow);
    integerConversionSemantics<double>(wide);
    integerConversionSemantics<float>(narrow);
    explicitFloatingConversions();
}

int main() {
    integerRemainderSemantics();
    binary32Semantics();
    numericSemantics();
    ownedOptionalSemantics();
    nullableSyntaxSemantics();
    extendedNumericSemantics();
}
