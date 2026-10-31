// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fundamentals.hpp"

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
#include <string>

using namespace pipelang_models;
using pipelang::model::Optional;
using pipelang::model::Result;

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

int main() {
    binary32Semantics();
    numericSemantics();
    ownedOptionalSemantics();
}
