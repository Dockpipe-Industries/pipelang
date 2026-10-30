// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Fundamentals.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <bit>
#include <cassert>
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

int main() {
    numericSemantics();
    ownedOptionalSemantics();
}
