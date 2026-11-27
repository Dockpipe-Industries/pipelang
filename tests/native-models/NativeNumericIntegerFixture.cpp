// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericIntegerFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericIntegerFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/IntegerQueries.pipe", R"pipe(module Numeric;
public model Class IntegerQueries {
    public pure int Echo(int value) => value;
    public pure int ZeroAdd(int value) => value + 0;
    public pure int LocalArithmetic() {
        int value = 5;
        return value + 2;
    }
    public pure Result<int, ClosedError> Success(int value) => Result<int, ClosedError>.Success(value);
    public pure Result<int, ClosedError> Forward(Result<int, ClosedError> value) => value;
    public pure int Extract(Result<int, ClosedError> value, int fallback) {
        if (value.IsSuccess)
            return value.Value;
        return fallback;
    }

    public pure Result<int, ClosedError> Convert(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat(value));
    }
    public pure Result<int, ClosedError> ConvertSingle(float32 value) {
        if (!float32.IsFinite(value) || value < -9223372036854775808.0f || value >= 9223372036854775808.0f)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat32(value));
    }
    public pure Result<int, ClosedError> LazyBoolean(float value) {
        if (float.IsFinite(value) && value >= -9223372036854775808.0 && value < 9223372036854775808.0)
            return Result<int, ClosedError>.Success(int.FromFloat(value));
        return Result<int, ClosedError>.Failure(ClosedError.Invalid);
    }
    public pure Result<int, ClosedError> Negated(float value) {
        if (!(float.IsFinite(value) && value >= -9223372036854775808.0 && value < 9223372036854775808.0))
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat(value));
    }
    public pure Result<int, ClosedError> EarlyReturns(float value) {
        if (!float.IsFinite(value))
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        if (value < -9223372036854775808.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        if (value >= 9223372036854775808.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat(value));
    }
    public pure Result<int, ClosedError> Joined(float value, bool select) {
        if (select) {
            if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
                return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        } else {
            if (!float.IsFinite(value))
                return Result<int, ClosedError>.Failure(ClosedError.Invalid);
            if (value < -9223372036854775808.0 || value >= 9223372036854775808.0)
                return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        }
        return Result<int, ClosedError>.Success(int.FromFloat(value));
    }
    public pure Result<int, ClosedError> AliasAfterGuard(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        float alias = value;
        return Result<int, ClosedError>.Success(int.FromFloat(alias));
    }
    public pure int Conditional(float value)
        => float.IsFinite(value) && value >= -9223372036854775808.0 && value < 9223372036854775808.0
            ? int.FromFloat(value) : 0;
    public pure bool CheckedComparison(float value)
        => float.IsFinite(value) && value >= -9223372036854775808.0 && value < 9223372036854775808.0
            && int.FromFloat(value) >= 0;

    public pure Result<int, ClosedError> EngineBound(float value) {
        if (!float.IsFinite(value) || value <= -2147483649.0 || value >= 2147483648.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat(value));
    }
    public pure Result<int, ClosedError> FloorSingle(float32 value) {
        float rounded = float.FromFloat32(float32.Floor(value));
        if (!float.IsFinite(rounded) || rounded <= -2147483649.0 || rounded >= 2147483648.0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(int.FromFloat(rounded));
    }

    public pure Result<int, ClosedError> Divide(int dividend, int divisor) {
        if (dividend < 0 || divisor <= 0)
            return Result<int, ClosedError>.Failure(ClosedError.Invalid);
        return Result<int, ClosedError>.Success(dividend / divisor);
    }
    public pure int ScaledDivide(int value, int factor, int divisor) {
        if (value < 0 || value > 1000000 || factor < 0 || factor > 1000000000 || divisor <= 0)
            return 0;
        return value * factor / divisor;
    }
    public pure int ReciprocalScale(int value, int factor) {
        if (value < 0 || value > 65535 || factor < 1 || factor > 99)
            return 0;
        factor = 10000 / factor;
        return value * factor / 100;
    }
    public pure int QuotientIndex(int dividend, int divisor) {
        if (dividend < 12 || dividend > 24 || divisor < 3 || divisor > 4)
            return 0;
        int quotient = dividend / divisor;
        Array<int> values = Array<int>.Create(6, 73);
        return values.At(quotient - 3);
    }
    public pure int DivisionAlias(int dividend, int divisor) {
        if (dividend < 0 || divisor <= 0)
            return 0;
        int alias = divisor;
        return dividend / alias;
    }
    public pure int DivisionJoin(int dividend, int divisor, bool select) {
        if (dividend < 0)
            return 0;
        if (select) {
            if (divisor < 1)
                return 0;
        } else {
            if (divisor < 2)
                return 0;
        }
        return dividend / divisor;
    }

    public pure int DivisionReversedGuard(int dividend, int divisor) {
        if (0 > dividend || 0 >= divisor)
            return 0;
        return dividend / divisor;
    }
    public pure int DivisionInclusiveGuard(int dividend, int divisor) {
        if (dividend >= 0 && divisor >= 1 && divisor <= 3)
            return dividend / divisor;
        return 0;
    }
    public pure int DivisionEndpointGuards(int dividend, int divisor) {
        if (dividend < -9223372036854775808 || divisor > 9223372036854775807)
            return 0;
        if (dividend < 0 || divisor <= 0)
            return 0;
        return dividend / divisor;
    }
    public pure int DivisionMaximumDivisor(int dividend, int divisor) {
        if (dividend < 0 || divisor < 9223372036854775807)
            return 0;
        int quotient = dividend / divisor;
        Array<int> values = Array<int>.Create(2, 91);
        return values.At(quotient);
    }

    public pure int IntervalAdmission(int value, int limit) {
        if (limit < 0 || limit > 4096 || value > limit)
            return -1;
        if (value < 0)
            return -2;
        return value * 4;
    }
    public pure int IntervalLess(int value, int limit) {
        if (limit < 0 || limit > 4096)
            return -1;
        if (value < limit) {
            if (value < 0)
                return -2;
            return value * 4;
        }
        return -3;
    }
    public pure int IntervalLessEqual(int value, int limit) {
        if (limit < 0 || limit > 4096)
            return -1;
        if (value <= limit) {
            if (value < 0)
                return -2;
            return value * 4;
        }
        return -3;
    }
    public pure int IntervalGreater(int value, int limit) {
        if (value < 0 || value > 4096)
            return -1;
        if (value > limit) {
            if (limit < 0)
                return -2;
            return limit * 4;
        }
        return -3;
    }
    public pure int IntervalGreaterEqual(int value, int limit) {
        if (value < 0 || value > 4096)
            return -1;
        if (value >= limit) {
            if (limit < 0)
                return -2;
            return limit * 4;
        }
        return -3;
    }
    public pure int IntervalJoin(int value, int limit, bool select) {
        if (limit < 0 || limit > 4096 || value < 0)
            return -1;
        if (select) {
            if (value > limit)
                return -1;
        } else {
            if (limit < value)
                return -1;
        }
        return value * 4;
    }
    public pure int IntervalEndpoints(int lower, int upper) {
        if (upper <= -9223372036854775808 && lower < upper)
            return 99;
        if (lower >= 9223372036854775807 && upper > lower)
            return 99;
        return 0;
    }
    public pure int IntervalMissingLower(int value, int limit) {
        if (limit < 0 || limit > 4096 || value > limit)
            return 0;
        return value * 4;
    }
    public pure int IntervalReassigned(int value, int limit, int replacement) {
        if (limit < 0 || limit > 4096 || value > limit || value < 0)
            return 0;
        value = replacement;
        return value * 4;
    }
    public pure int IntervalIncompleteJoin(int value, int limit, bool select) {
        if (limit < 0 || limit > 4096 || value < 0)
            return 0;
        if (select) {
            if (value > limit)
                return 0;
        }
        return value * 4;
    }

    public pure int UncheckedDivision(int dividend, int divisor) => dividend / divisor;
    public pure int DivisionAllowsZero(int dividend, int divisor) {
        if (dividend < 0 || divisor < 0)
            return 0;
        return dividend / divisor;
    }
    public pure int NegativeDividend(int dividend, int divisor) {
        if (dividend >= 0 || divisor <= 0)
            return 0;
        return dividend / divisor;
    }
    public pure int NegativeDivisor(int dividend, int divisor) {
        if (dividend < 0 || divisor >= 0)
            return 0;
        return dividend / divisor;
    }
    public pure int DivisionOverflow(int dividend, int divisor) {
        if (dividend != -9223372036854775808 || divisor != -1)
            return 0;
        return dividend / divisor;
    }
    public pure int DivisionReassigned(int dividend, int divisor) {
        if (dividend < 0 || divisor <= 0)
            return 0;
        divisor = 0;
        return dividend / divisor;
    }
    public pure int DivisionAliasBefore(int dividend, int divisor) {
        int alias = divisor;
        if (dividend < 0 || divisor <= 0)
            return 0;
        return dividend / alias;
    }
    private pure int UncheckedDivisionHelper(int dividend, int divisor) => dividend / divisor;
    public pure int DivisionGuardedCaller(int dividend, int divisor) {
        if (dividend < 0 || divisor <= 0)
            return 0;
        return UncheckedDivisionHelper(dividend, divisor);
    }
    public pure int DivisionIncompleteJoin(int dividend, int divisor, bool select) {
        if (dividend < 0)
            return 0;
        if (select) {
            if (divisor <= 0)
                return 0;
        }
        return dividend / divisor;
    }
    public pure int DivisionReplacement(int dividend, int divisor, int replacement) {
        if (dividend < 0 || divisor <= 0)
            return 0;
        divisor = replacement;
        return dividend / divisor;
    }
    public pure int DivisionBooleanAlias(int dividend, int divisor) {
        bool admitted = dividend >= 0 && divisor > 0;
        if (admitted)
            return dividend / divisor;
        return 0;
    }
    public pure int DivisionImpossibleGuard(int dividend, int divisor) {
        if (dividend < 0)
            return 0;
        if (divisor > 9223372036854775807)
            divisor = 1;
        return dividend / divisor;
    }
    public pure int ProductOverflowBeforeDivision(int value) {
        if (value <= 0)
            return 0;
        return value * 2 / value;
    }

    public pure int Unchecked(float value) => int.FromFloat(value);
    public pure int FiniteOnly(float value) {
        if (!float.IsFinite(value))
            return 0;
        return int.FromFloat(value);
    }
    public pure int MissingFinite(float value) {
        if (value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        return int.FromFloat(value);
    }
    public pure int InclusiveUpper(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value > 9223372036854775808.0)
            return 0;
        return int.FromFloat(value);
    }
    // The authored signed64 MAX spelling rounds upward to 2^63 in binary32.
    // A finite/lower-bound guard cannot make that inclusive upper endpoint safe.
    public pure int RoundedSingleUpper(float32 value) {
        if (!float32.IsFinite(value) || value < -9223372036854775808.0f || value > 9223372036854775807.0f)
            return 0;
        return int.FromFloat32(value);
    }
    public pure int BelowLower(float value) {
        if (!float.IsFinite(value) || value < -18446744073709551616.0 || value >= 9223372036854775808.0)
            return 0;
        return int.FromFloat(value);
    }
    public pure int Reassigned(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        value = float.PositiveInfinity;
        return int.FromFloat(value);
    }
    public pure int ReassignedFromParameter(float value, float replacement) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        value = replacement;
        return int.FromFloat(value);
    }
    public pure int MutatedAfterGuard(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        value = value * 2.0;
        return int.FromFloat(value);
    }
    public pure int ConditionalReplacement(float value, float replacement, bool change) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        if (change)
            value = replacement;
        return int.FromFloat(value);
    }

    private pure int UncheckedHelper(float value) => int.FromFloat(value);

    public pure int GuardedCaller(float value) {
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        return UncheckedHelper(value);
    }

    public pure int AliasBeforeGuard(float value) {
        float alias = value;
        if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
            return 0;
        return int.FromFloat(alias);
    }
    public pure int BooleanAlias(float value) {
        bool safe = float.IsFinite(value) && value >= -9223372036854775808.0 && value < 9223372036854775808.0;
        if (safe)
            return int.FromFloat(value);
        return 0;
    }
    public pure int IncompleteJoin(float value, bool select) {
        if (select) {
            if (!float.IsFinite(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
                return 0;
        }
        return int.FromFloat(value);
    }
    public pure int Overflow(int value) => value + 1;
    public pure int Multiply(int value) => value * 2;
    public pure float32 UnknownIndex(int index) {
        Array<float32> values = Array<float32>.Create(4, 1.0f);
        return values.At(index);
    }
    public pure float32 UnknownArrayIndex(int value) {
        Array<int> indices = Array<int>.Create(1, value);
        int index = indices.At(0);
        Array<float32> values = Array<float32>.Create(4, 1.0f);
        return values.At(index);
    }
    public pure int UncheckedResult(Result<int, ClosedError> value) => value.Value;
}
)pipe");
}

void verifyNativeNumericIntegerExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.IntegerQueries")
            methods.insert(item.at("method").get<std::string>());
    }
    require(methods == std::set<std::string>{"Echo", "ZeroAdd", "LocalArithmetic", "Success", "Forward", "Extract",
        "Convert", "ConvertSingle", "LazyBoolean", "Negated", "EarlyReturns", "Joined", "AliasAfterGuard",
        "Conditional", "CheckedComparison", "EngineBound", "FloorSingle", "Divide", "ScaledDivide",
        "ReciprocalScale", "QuotientIndex", "DivisionAlias", "DivisionJoin", "DivisionReversedGuard",
        "DivisionInclusiveGuard", "DivisionEndpointGuards", "DivisionMaximumDivisor", "IntervalAdmission",
        "IntervalLess", "IntervalLessEqual", "IntervalGreater", "IntervalGreaterEqual", "IntervalJoin", "IntervalEndpoints"},
        "integer numeric projection lost a proven conversion or admitted an unsafe scalar fact");
}

std::string nativeNumericIntegerConsumer()
{
    return R"cpp(
template<class Query>
bool verifyIntegerProjection()
{
    using Result = decltype(Query::Success(0));
    using Error = std::remove_cvref_t<decltype(std::declval<Result>().error())>;
    static_assert(std::is_same_v<decltype(Query::Echo(0)), std::int64_t>);
    static_assert(noexcept(Query::Echo(0)));
    static_assert(noexcept(Query::ZeroAdd(0)));
    static_assert(noexcept(Query::LocalArithmetic()));
    static_assert(noexcept(Query::Success(0)));
    static_assert(noexcept(Query::Forward(std::declval<Result>())));
    static_assert(noexcept(Query::Extract(std::declval<Result>(), 0)));
    static_assert(noexcept(Query::Convert(0.0)));
    static_assert(noexcept(Query::ConvertSingle(0.0f)));
    static_assert(noexcept(Query::LazyBoolean(0.0)));
    static_assert(noexcept(Query::Negated(0.0)));
    static_assert(noexcept(Query::EarlyReturns(0.0)));
    static_assert(noexcept(Query::Joined(0.0, false)));
    static_assert(noexcept(Query::AliasAfterGuard(0.0)));
    static_assert(noexcept(Query::Conditional(0.0)));
    static_assert(noexcept(Query::CheckedComparison(0.0)));
    static_assert(noexcept(Query::EngineBound(0.0)));
    static_assert(noexcept(Query::FloorSingle(0.0f)));
    static_assert(noexcept(Query::DivisionReversedGuard(0, 1)));
    static_assert(noexcept(Query::DivisionInclusiveGuard(0, 1)));
    static_assert(noexcept(Query::DivisionEndpointGuards(0, 1)));
    static_assert(noexcept(Query::DivisionMaximumDivisor(0, 1)));
    static_assert(noexcept(Query::Divide(0, 1)));
    static_assert(noexcept(Query::ScaledDivide(0, 0, 1)));
    static_assert(noexcept(Query::ReciprocalScale(0, 1)));
    static_assert(noexcept(Query::QuotientIndex(12, 3)));
    static_assert(noexcept(Query::DivisionAlias(0, 1)));
    static_assert(noexcept(Query::DivisionJoin(0, 1, false)));
    static_assert(noexcept(Query::IntervalAdmission(0, 1)));
    static_assert(Query::IntervalAdmission(4096, 4096) == 16384);
    static_assert(Query::IntervalLess(4095, 4096) == 16380);
    static_assert(Query::IntervalLessEqual(4096, 4096) == 16384);
    static_assert(Query::IntervalGreater(4096, 4095) == 16380);
    static_assert(Query::IntervalGreaterEqual(4096, 4096) == 16384);
    static_assert(Query::IntervalJoin(4096, 4096, true) == 16384);
    static_assert(Query::IntervalJoin(4096, 4096, false) == 16384);
    static_assert(Query::IntervalEndpoints(INT64_MIN, INT64_MIN) == 0);
    static_assert(Query::IntervalEndpoints(INT64_MAX, INT64_MAX) == 0);
    const auto scaleAdmitted = [](std::int64_t candidate) {
        if (candidate < 0)
            return INT64_C(-2);
        return candidate * 4;
    };
    for (const auto value : {INT64_MIN, INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(4095),
                             INT64_C(4096), INT64_C(4097), INT64_MAX}) {
        for (const auto limit : {INT64_MIN, INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(4095),
                                 INT64_C(4096), INT64_C(4097), INT64_MAX}) {
            const bool limitAdmitted = limit >= 0 && limit <= 4096;
            const bool valueAdmitted = value >= 0 && value <= 4096;
            std::int64_t admission = -1;
            if (limitAdmitted && value <= limit)
                admission = value < 0 ? -2 : value * 4;
            std::int64_t less = -1;
            std::int64_t lessEqual = -1;
            if (limitAdmitted) {
                less = value < limit ? scaleAdmitted(value) : -3;
                lessEqual = value <= limit ? scaleAdmitted(value) : -3;
            }
            std::int64_t greater = -1;
            std::int64_t greaterEqual = -1;
            if (valueAdmitted) {
                greater = value > limit ? scaleAdmitted(limit) : -3;
                greaterEqual = value >= limit ? scaleAdmitted(limit) : -3;
            }
            std::int64_t joined = -1;
            if (limitAdmitted && value >= 0 && value <= limit)
                joined = value * 4;
            if (Query::IntervalAdmission(value, limit) != admission ||
                Query::IntervalLess(value, limit) != less || Query::IntervalLessEqual(value, limit) != lessEqual ||
                Query::IntervalGreater(value, limit) != greater || Query::IntervalGreaterEqual(value, limit) != greaterEqual ||
                Query::IntervalJoin(value, limit, true) != joined || Query::IntervalJoin(value, limit, false) != joined ||
                Query::IntervalEndpoints(value, limit) != 0)
                return false;
        }
    }
    for (const auto dividend : {INT64_C(0), INT64_C(1), INT64_C(17), INT64_C(1000000000000000),
                               std::numeric_limits<std::int64_t>::max()}) {
        for (const auto divisor : {INT64_C(1), INT64_C(2), INT64_C(3), INT64_C(19),
                                  std::numeric_limits<std::int64_t>::max()}) {
            const auto actual = Query::Divide(dividend, divisor);
            if (!actual.is_success() || actual.value() != dividend / divisor ||
                Query::DivisionAlias(dividend, divisor) != dividend / divisor ||
                Query::DivisionReversedGuard(dividend, divisor) != dividend / divisor ||
                Query::DivisionInclusiveGuard(dividend, divisor) != (divisor <= 3 ? dividend / divisor : 0) ||
                Query::DivisionEndpointGuards(dividend, divisor) != dividend / divisor ||
                Query::DivisionMaximumDivisor(dividend, divisor) !=
                    (divisor == std::numeric_limits<std::int64_t>::max() ? 91 : 0) ||
                Query::DivisionJoin(dividend, divisor, true) != dividend / divisor ||
                Query::DivisionJoin(dividend, divisor, false) != (divisor < 2 ? 0 : dividend / divisor))
                return false;
        }
    }
    for (const auto invalid : {Query::Divide(-1, 1), Query::Divide(1, 0), Query::Divide(1, -1),
                               Query::Divide(std::numeric_limits<std::int64_t>::min(), -1)}) {
        if (!invalid.is_failure() || invalid.error() != Error::Invalid)
            return false;
    }
    for (const auto value : {INT64_C(0), INT64_C(1), INT64_C(12345), INT64_C(1000000)}) {
        for (const auto factor : {INT64_C(0), INT64_C(1), INT64_C(913), INT64_C(1000000000)}) {
            for (const auto divisor : {INT64_C(1), INT64_C(3), INT64_C(100), std::numeric_limits<std::int64_t>::max()}) {
                if (Query::ScaledDivide(value, factor, divisor) != value * factor / divisor)
                    return false;
            }
        }
    }
    for (std::int64_t factor = 1; factor < 100; ++factor) {
        for (const auto value : {INT64_C(0), INT64_C(1), INT64_C(32768), INT64_C(65535)}) {
            if (Query::ReciprocalScale(value, factor) != value * (10000 / factor) / 100)
                return false;
        }
    }
    for (std::int64_t dividend = 12; dividend <= 24; ++dividend) {
        for (std::int64_t divisor = 3; divisor <= 4; ++divisor) {
            if (Query::QuotientIndex(dividend, divisor) != 73)
                return false;
        }
    }
    if (Query::LocalArithmetic() != 7)
        return false;
    for (const auto value : {std::numeric_limits<std::int64_t>::min(), INT64_C(-1), INT64_C(0),
                            INT64_C(1), std::numeric_limits<std::int64_t>::max()}) {
        const auto success = Query::Success(value);
        const auto forwarded = Query::Forward(success);
        if (Query::Echo(value) != value || Query::ZeroAdd(value) != value ||
            !success.is_success() || success.value() != value || !forwarded.is_success() ||
            forwarded.value() != value || Query::Extract(forwarded, 42) != value)
            return false;
    }
    for (const auto& result : {Result::Failure(Error::Invalid), Result::Cancelled(Error::Interrupted)}) {
        const auto forwarded = Query::Forward(result);
        if (forwarded.outcome() != result.outcome() || forwarded.error() != result.error() ||
            Query::Extract(forwarded, 42) != 42)
            return false;
    }
    const auto check = [](const Result& result, double value, bool safe) {
        return safe ? result.is_success() && result.value() == static_cast<std::int64_t>(value)
                    : result.is_failure() && result.error() == Error::Invalid;
    };
    for (const double value : {0.0, -0.0, 1.75, -1.75, -0x1p63, std::nextafter(-0x1p63, 0.0),
                              std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity()),
                              std::nextafter(0x1p63, 0.0), 0x1p63,
                              std::nextafter(0x1p63, std::numeric_limits<double>::infinity()),
                              std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()}) {
        const bool safe = std::isfinite(value) && value >= -0x1p63 && value < 0x1p63;
        if (!check(Query::Convert(value), value, safe) || !check(Query::LazyBoolean(value), value, safe) ||
            !check(Query::Negated(value), value, safe) || !check(Query::EarlyReturns(value), value, safe) ||
            !check(Query::Joined(value, false), value, safe) || !check(Query::Joined(value, true), value, safe) ||
            !check(Query::AliasAfterGuard(value), value, safe) ||
            Query::Conditional(value) != (safe ? static_cast<std::int64_t>(value) : 0) ||
            Query::CheckedComparison(value) != (safe && static_cast<std::int64_t>(value) >= 0))
            return false;
    }
    for (const float value : {0.0f, -0.0f, 1.75f, -1.75f, -0x1p63f, std::nextafter(-0x1p63f, 0.0f),
                             std::nextafter(-0x1p63f, -std::numeric_limits<float>::infinity()),
                             std::nextafter(0x1p63f, 0.0f), 0x1p63f,
                             std::nextafter(0x1p63f, std::numeric_limits<float>::infinity()),
                             std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()}) {
        const bool safe = std::isfinite(value) && value >= -0x1p63f && value < 0x1p63f;
        if (!check(Query::ConvertSingle(value), value, safe))
            return false;
    }
    for (const double value : {-2147483649.0, std::nextafter(-2147483649.0, 0.0), -2147483648.75,
                              -2147483648.0, 0.0, 2147483647.75, std::nextafter(2147483648.0, 0.0),
                              2147483648.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        const bool safe = std::isfinite(value) && value > -2147483649.0 && value < 2147483648.0;
        if (!check(Query::EngineBound(value), value, safe))
            return false;
    }
    for (const float value : {-2147483648.0f, -1.25f, -0.0f, 0.0f, 1.25f,
                             std::nextafter(2147483648.0f, 0.0f), 2147483648.0f,
                             std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const double rounded = static_cast<double>(std::floor(value));
        const bool safe = std::isfinite(rounded) && rounded > -2147483649.0 && rounded < 2147483648.0;
        if (!check(Query::FloorSingle(value), rounded, safe))
            return false;
    }
    return true;
}

bool verifyNumericIntegers()
{
    return verifyIntegerProjection<numeric_fixture::native_numeric::Numeric::IntegerQueries>() &&
        verifyIntegerProjection<numeric_compiled_fixture::native_numeric::Numeric::IntegerQueries>();
}
)cpp";
}
