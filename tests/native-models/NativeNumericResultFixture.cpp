// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericResultFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericResultFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/NumericResult.pipe", R"pipe(module Numeric;
public Record NumericResult {
    public float32 First;
    public float Second;
    public bool Valid;
}
)pipe");
    atomicWrite(output / "src/DefaultedResult.pipe", R"pipe(module Numeric;
public Record DefaultedResult {
    public float32 Value = float32.Abs(-2.0f);
    public bool Valid = true;
}
public Record UnsafeDefault {
    public float32 Value = List<float32>.Empty().At(0);
}
)pipe");
    atomicWrite(output / "src/NestedResult.pipe", R"pipe(module Numeric;
public Record NestedResult {
    public NumericResult Value;
}
)pipe");
    atomicWrite(output / "src/ResultQueries.pipe", R"pipe(module Numeric;
public model Class ResultQueries {
    private pure NumericResult Make(float32 value)
        => new NumericResult { First = -value, Second = 2.0, Valid = true };

    public pure NumericResult Evaluate(float32 value, bool enabled) {
        if (!enabled)
            return new NumericResult { First = -0.0f };
        NumericResult result = Make(value);
        return result;
    }

    public pure DefaultedResult Defaulted() => new DefaultedResult { };
    public pure UnsafeDefault HiddenDefault() => new UnsafeDefault { Value = 1.0f };
    public pure NumericResult Copy(NumericResult value) => value;
    public pure NumericResult Positional(float32 value) => NumericResult(value, 3.0, true);
    public pure NumericResult Choose(NumericResult value, bool enabled)
        => enabled ? value : new NumericResult { Valid = false };

    public pure NestedResult Nested()
        => new NestedResult { Value = new NumericResult { First = 1.0f } };
    public pure UnsafeParameters Unsafe() => new UnsafeParameters { };
    public pure NumericResult Mutate(NumericResult value) {
        value.First = 3.0f;
        return value;
    }
    public pure NumericResult Reassign(NumericResult value) {
        value = Make(1.0f);
        return value;
    }
}
)pipe");
    atomicWrite(output / "src/ClosedAdmission.pipe", R"pipe(module Numeric;
public Enum ClosedError { None, Invalid, Interrupted }
public Record ClosedParameters {
    public float32 Value;
    public uint32 Word;
    public ClosedError Error;
    public bool Enabled;
}
public Record NestedClosedParameters {
    public ClosedParameters Value;
}
public model Class ClosedQueries {
    public pure uint32 Bits(float32 value) => float32.ToBits(value);
    public pure float32 FromBits(uint32 word) => float32.FromBits(word);
    public pure uint32 Mask(uint32 word) => (word & 0x7fffffffu) | 0u;
    public pure uint32 Toggle(uint32 word) => word ^ 0x80000000u;
    public pure uint32 Invert(uint32 word) => ~word;
    public pure uint32 Wrap(uint32 word) => uint32.WrappingAdd(word, 1u);
    public pure ClosedError EchoError(ClosedError error) => error;

    private pure bool FinitePositive(uint32 word) => word > 0u && word < 0x7f800000u;

    public pure Result<ClosedParameters, ClosedError> Validate(float32 value, bool enabled) {
        if (!enabled)
            return Result<ClosedParameters, ClosedError>.Cancelled(ClosedError.Interrupted);
        uint32 word = float32.ToBits(value);
        if (!FinitePositive(word))
            return Result<ClosedParameters, ClosedError>.Failure(ClosedError.Invalid);
        return Result<ClosedParameters, ClosedError>.Success(new ClosedParameters {
            Value = value, Word = word, Error = ClosedError.None, Enabled = true
        });
    }

    public pure Result<ClosedParameters, ClosedError> Forward(Result<ClosedParameters, ClosedError> result)
        => result;
    public pure Result<ClosedParameters, ClosedError> Choose(Result<ClosedParameters, ClosedError> result, bool enabled)
        => enabled ? result : Result<ClosedParameters, ClosedError>.Cancelled();
    public pure bool Succeeded(Result<ClosedParameters, ClosedError> result) => result.IsSuccess;
    public pure bool Failed(Result<ClosedParameters, ClosedError> result) => result.IsFailure;
    public pure bool Cancelled(Result<ClosedParameters, ClosedError> result) => result.IsCancelled;
    public pure ClosedError ErrorOf(Result<ClosedParameters, ClosedError> result) => result.Error;
    public pure Result<void, ClosedError> Complete(bool enabled)
        => enabled ? Result<void, ClosedError>.Success() : Result<void, ClosedError>.Failure(ClosedError.Invalid);
    public pure Result<uint32, ClosedError> Word(uint32 value)
        => Result<uint32, ClosedError>.Success(value);

    public pure uint32 Add(uint32 left, uint32 right) => left + right;
    public pure uint32 Subtract(uint32 left, uint32 right) => left - right;
    public pure uint32 Multiply(uint32 left, uint32 right) => left * right;
    public pure uint32 Divide(uint32 left, uint32 right) => left / right;
    public pure uint32 Remainder(uint32 left, uint32 right) => left % right;
    public pure uint32 Shift(uint32 value) => value << 1;
    public pure uint32 ShiftRight(uint32 value) => value >> 1;
    public pure uint32 Convert(float32 value) => uint32.FromFloat32(value);
    public pure float32 NumericConvert(uint32 value) => float32.FromUInt32(value);
    public pure ClosedParameters Unwrap(Result<ClosedParameters, ClosedError> result) => result.Value;
    public pure float32 TransitiveUnwrap(Result<ClosedParameters, ClosedError> result) => Unwrap(result).Value;
    public pure Result<float32, string> AllocatingError(float32 value)
        => Result<float32, string>.Failure("invalid value");
    public pure Result<Result<uint32, ClosedError>, ClosedError> NestedResult(uint32 value)
        => Result<Result<uint32, ClosedError>, ClosedError>.Success(Word(value));
    public pure Result<NestedClosedParameters, ClosedError> NestedRecord()
        => Result<NestedClosedParameters, ClosedError>.Success(new NestedClosedParameters { });
    public pure Result<List<uint32>, ClosedError> ReferenceChannel()
        => Result<List<uint32>, ClosedError>.Success(List<uint32>.Empty());
}
)pipe");
    atomicWrite(output / "src/ResultComposition.pipe", R"pipe(module Numeric;
public model Class CompositionQueries {
    private pure Result<uint32, ClosedError> MakeWord(uint32 value, bool enabled, bool cancelled) {
        if (cancelled)
            return Result<uint32, ClosedError>.Cancelled(ClosedError.Interrupted);
        if (!enabled)
            return Result<uint32, ClosedError>.Failure(ClosedError.Invalid);
        return Result<uint32, ClosedError>.Success(value);
    }

    private pure Result<ClosedParameters, ClosedError> MakeRecord(float32 value, bool enabled, bool cancelled) {
        Result<uint32, ClosedError> result = MakeWord(float32.ToBits(value), enabled, cancelled);
        if (result.IsFailure)
            return Result<ClosedParameters, ClosedError>.Failure(result.Error);
        if (result.IsCancelled)
            return Result<ClosedParameters, ClosedError>.Cancelled(result.Error);
        return Result<ClosedParameters, ClosedError>.Success(new ClosedParameters {
            Value = value, Word = result.Value, Error = ClosedError.None, Enabled = true
        });
    }

    public pure Result<uint32, ClosedError> Scalar(uint32 value, bool enabled, bool cancelled) {
        Result<uint32, ClosedError> result = MakeWord(value, enabled, cancelled);
        if (!result.IsSuccess)
            return result;
        return Result<uint32, ClosedError>.Success(uint32.WrappingAdd(result.Value, 1u));
    }

    public pure Result<ClosedParameters, ClosedError> Record(float32 value, bool enabled, bool cancelled) {
        Result<ClosedParameters, ClosedError> result = MakeRecord(value, enabled, cancelled);
        if (!result.IsSuccess)
            return result;
        ClosedParameters parameters = result.Value;
        return Result<ClosedParameters, ClosedError>.Success(new ClosedParameters {
            Value = parameters.Value, Word = uint32.WrappingAdd(parameters.Word, 1u),
            Error = parameters.Error, Enabled = parameters.Enabled
        });
    }

    public pure uint32 Branch(Result<uint32, ClosedError> result, uint32 fallback) {
        if (result.IsSuccess)
            return result.Value;
        return fallback;
    }

    public pure uint32 NegatedStatuses(Result<uint32, ClosedError> result) {
        if (!result.IsFailure && !result.IsCancelled)
            return result.Value;
        return 0u;
    }

    public pure Result<ClosedParameters, ClosedError> EarlyStatuses(Result<ClosedParameters, ClosedError> result) {
        if (result.IsFailure)
            return result;
        if (result.IsCancelled)
            return result;
        return Result<ClosedParameters, ClosedError>.Success(result.Value);
    }

    public pure uint32 Joined(Result<uint32, ClosedError> result, bool select) {
        if (select) {
            if (!result.IsSuccess)
                return 0u;
        } else {
            if (result.IsFailure || result.IsCancelled)
                return 0u;
        }
        return result.Value;
    }

    public pure bool ShortCircuit(Result<uint32, ClosedError> result, uint32 expected)
        => result.IsSuccess && result.Value == expected;
    public pure uint32 Conditional(Result<uint32, ClosedError> result)
        => result.IsSuccess ? result.Value : 0u;

    private pure uint32 CheckedValue(Result<uint32, ClosedError> result) {
        if (result.IsSuccess)
            return result.Value;
        return 0u;
    }

    public pure uint32 GuardedHelper(Result<uint32, ClosedError> result) => CheckedValue(result);

    public pure uint32 AliasAfterGuard(Result<uint32, ClosedError> result) {
        if (!result.IsSuccess)
            return 0u;
        Result<uint32, ClosedError> alias = result;
        return alias.Value;
    }

    public pure uint32 Unchecked(Result<uint32, ClosedError> result) => result.Value;
    public pure uint32 IncompleteGuard(Result<uint32, ClosedError> result) {
        if (result.IsFailure)
            return 0u;
        return result.Value;
    }
    public pure uint32 UnsafeDisjunction(Result<uint32, ClosedError> result, bool enabled) {
        if (result.IsSuccess || enabled)
            return result.Value;
        return 0u;
    }
    public pure uint32 UnsafeJoin(Result<uint32, ClosedError> result, bool select) {
        if (select) {
            if (!result.IsSuccess)
                return 0u;
        }
        return result.Value;
    }
    private pure uint32 UnsafeHelper(Result<uint32, ClosedError> result) => result.Value;
    public pure uint32 GuardedCaller(Result<uint32, ClosedError> result) {
        if (!result.IsSuccess)
            return 0u;
        return UnsafeHelper(result);
    }
    public pure uint32 ReusedLocal(bool enabled) {
        if (enabled) {
            Result<uint32, ClosedError> result = Result<uint32, ClosedError>.Success(1u);
            if (!result.IsSuccess)
                return 0u;
        }
        Result<uint32, ClosedError> result = Result<uint32, ClosedError>.Failure(ClosedError.Invalid);
        return result.Value;
    }
    public pure uint32 BoolAlias(Result<uint32, ClosedError> result) {
        bool successful = result.IsSuccess;
        if (successful)
            return result.Value;
        return 0u;
    }
    public pure uint32 AliasBeforeGuard(Result<uint32, ClosedError> result) {
        Result<uint32, ClosedError> alias = result;
        if (result.IsSuccess)
            return alias.Value;
        return 0u;
    }
}
)pipe");
    std::string costly = "module Numeric;\npublic model Class CostlyResults {\n";
    costly += "    private pure LargeParameters Level0(bool enabled) => new LargeParameters { };\n";
    for (int level = 1; level <= 9; ++level) {
        const auto previous = "Level" + std::to_string(level - 1) + "(enabled)";
        costly += "    private pure LargeParameters Level" + std::to_string(level) +
            "(bool enabled) => enabled ? " + previous + " : " + previous + ";\n";
    }
    costly += "    public pure LargeParameters Evaluate(bool enabled) => Level9(enabled);\n}\n";
    costly += "public model Class CostlyClosedResults {\n";
    costly += "    private pure Result<LargeParameters, ClosedError> Level0(bool enabled) => Result<LargeParameters, ClosedError>.Success(new LargeParameters { });\n";
    for (int level = 1; level <= 9; ++level) {
        const auto previous = "Level" + std::to_string(level - 1) + "(enabled)";
        costly += "    private pure Result<LargeParameters, ClosedError> Level" + std::to_string(level) +
            "(bool enabled) => enabled ? " + previous + " : " + previous + ";\n";
    }
    costly += "    public pure Result<LargeParameters, ClosedError> Evaluate(bool enabled) => Level9(enabled);\n}\n";
    atomicWrite(output / "src/CostlyResults.pipe", costly);
}

void verifyNativeNumericResultExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.CostlyResults" && item.at("owner") != "Numeric.CostlyClosedResults",
                "record or closed Result payloads multiplied the work allowance");
        if (item.at("owner") != "Numeric.ResultQueries")
            continue;
        const auto method = item.at("method").get<std::string>();
        std::string expected = "Numeric.NumericResult";
        if (method == "Defaulted")
            expected = "Numeric.DefaultedResult";
        else if (method == "Nested")
            expected = "Numeric.NestedResult";
        require(item.at("returnType") == expected, "numeric record return lost its source-qualified type");
        methods.insert(item.at("method").get<std::string>());
    }
    require(methods == std::set<std::string>{"Evaluate", "Copy", "Positional", "Choose", "Defaulted", "Nested"},
            "numeric result exports missing or unsafe results admitted");
    std::set<std::string> closedMethods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.ClosedQueries")
            closedMethods.insert(item.at("method").get<std::string>());
    }
    require(closedMethods == std::set<std::string>{"Bits", "FromBits", "Mask", "Toggle", "Invert", "Wrap",
        "EchoError", "Validate", "Forward", "Choose", "Succeeded", "Failed", "Cancelled", "ErrorOf", "Complete", "Word", "NestedRecord"},
        "closed numeric admission lost typed results or admitted a throwing/allocating operation");

    std::set<std::string> compositionMethods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.CompositionQueries")
            compositionMethods.insert(item.at("method").get<std::string>());
    }
    require(compositionMethods == std::set<std::string>{"Scalar", "Record", "Branch", "NegatedStatuses",
        "EarlyStatuses", "Joined", "ShortCircuit", "Conditional", "GuardedHelper", "AliasAfterGuard"},
        "guarded Result composition lost a safe export or admitted an unproven payload read");

    auto invalid = manifest;
    for (auto& record : invalid["records"]) {
        if (record.value("sourceName", "") == "Numeric.ClosedParameters")
            record["fields"][1]["type"] = "List<uint32>";
    }
    bool refused = false;
    try {
        admitModel(invalid, manifest.at("namespace").get<std::string>());
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "closed Result payload admitted a reference-bearing field");

    invalid = manifest;
    for (auto& declaration : invalid["enums"]) {
        if (declaration.value("sourceName", "") == "Numeric.ClosedError")
            declaration["sourceName"] = "Numeric.OtherError";
    }
    // Remove both recognized identities so qualified enum errors cannot be
    // mistaken for arbitrary textual channels by manifest admission.
    for (auto& declaration : invalid["enums"]) {
        if (declaration.value("sourceName", "") == "Numeric.OtherError")
            declaration["name"] = "OtherError";
    }
    refused = false;
    try {
        admitModel(invalid, manifest.at("namespace").get<std::string>());
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "closed Result admitted an undeclared error enum");
}

std::string nativeNumericResultConsumer()
{
    return R"cpp(
template<class Query>
concept ExposesPrivateMake = requires { Query::Make(0.0f); };

template<class Query, class Result>
bool verifyResultProjection()
{
    static_assert(!ExposesPrivateMake<Query>);
    static_assert(noexcept(Query::Evaluate(1.0f, true)));
    static_assert(std::is_same_v<decltype(Query::Evaluate(1.0f, true)), Result>);
    const auto initialized = Query::Defaulted();
    if (initialized.Value != 2.0f || !initialized.Valid)
        return false;
    const Result made = Query::Evaluate(2.0f, true);
    if (made.First != -2.0f || made.Second != 2.0 || !made.Valid)
        return false;
    Result copy = Query::Copy(made);
    copy.First = 9.0f;
    if (copy.First != 9.0f || copy.Second != 2.0 || !copy.Valid || made.First != -2.0f)
        return false;
    const auto defaulted = Query::Evaluate(1.0f, false);
    if (std::bit_cast<std::uint32_t>(defaulted.First) != 0x80000000U ||
        defaulted.Second != 0.0 || defaulted.Valid)
        return false;
    const auto positional = Query::Positional(4.0f);
    if (positional.First != 4.0f || positional.Second != 3.0 || !positional.Valid)
        return false;
    const auto chosen = Query::Choose(made, true);
    const auto empty = Query::Choose(made, false);
    return chosen.First == -2.0f && chosen.Valid && empty.First == 0.0f && !empty.Valid;
}

template<class Query>
bool verifyClosedProjection()
{
    using Error = decltype(Query::EchoError({}));
    static_assert(noexcept(Query::Bits(0.0f)));
    static_assert(noexcept(Query::FromBits(0u)));
    static_assert(noexcept(Query::Validate(1.0f, true)));
    static_assert(noexcept(Query::Forward(Query::Validate(1.0f, true))));
    const auto made = Query::Validate(std::bit_cast<float>(0x00000001u), true);
    if (!Query::Succeeded(made) || Query::Failed(made) || Query::Cancelled(made) ||
        made.value().Word != 1u || Query::Bits(made.value().Value) != 1u ||
        made.value().Error != Error::None || !made.value().Enabled)
        return false;
    const auto failed = Query::Validate(std::bit_cast<float>(0x80000000u), true);
    const auto cancelled = Query::Validate(1.0f, false);
    if (!Query::Failed(failed) || Query::ErrorOf(failed) != Error::Invalid ||
        !Query::Cancelled(cancelled) || Query::ErrorOf(cancelled) != Error::Interrupted)
        return false;
    if (!Query::Succeeded(Query::Forward(made)) || !Query::Failed(Query::Forward(failed)) ||
        !Query::Cancelled(Query::Forward(cancelled)) || !Query::Cancelled(Query::Choose(made, false)) ||
        Query::ErrorOf(Query::Choose(made, false)) != Error::None ||
        !Query::Complete(true).is_success() || Query::Complete(false).error() != Error::Invalid ||
        Query::Word(0xffffffffu).value() != 0xffffffffu)
        return false;
    if (Query::Mask(0xffffffffu) != 0x7fffffffu || Query::Toggle(0u) != 0x80000000u ||
        Query::Invert(0u) != 0xffffffffu || Query::Wrap(0xffffffffu) != 0u ||
        Query::EchoError(Error::Interrupted) != Error::Interrupted)
        return false;
    for (const auto word : {0u, 0x80000000u, 1u, 0x80000001u, 0x7f7fffffu,
                           0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f812345u}) {
        const float value = std::bit_cast<float>(word);
        if (Query::Bits(value) != word || std::bit_cast<std::uint32_t>(Query::FromBits(word)) != word)
            return false;
        const bool expected = word > 0u && word < 0x7f800000u;
        if (Query::Succeeded(Query::Validate(value, true)) != expected)
            return false;
    }
    return true;
}

bool verifyClosedNumericResults()
{
    return verifyClosedProjection<numeric_fixture::native_numeric::Numeric::ClosedQueries>() &&
        verifyClosedProjection<numeric_compiled_fixture::native_numeric::Numeric::ClosedQueries>();
}

bool verifyHostileNumericEnvironment()
{
    // Only raw-bit/unsigned operations run here. They promise exact transport
    // without floating arithmetic, mode admission or floating-state mutation.
    std::fenv_t saved;
    if (std::fegetenv(&saved) != 0 || std::fesetround(FE_DOWNWARD) != 0)
        return false;
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_INEXACT);
#if defined(__SSE2__)
    const auto previousControl = _mm_getcsr();
    _mm_setcsr(previousControl | 0x8040u); // Enable flush-to-zero and denormals-are-zero.
    const auto expectedControl = _mm_getcsr();
#endif
    const auto expectedFlags = std::fetestexcept(FE_ALL_EXCEPT);
    const bool valuesMatch = verifyClosedNumericResults();
    const bool roundingMatches = std::fegetround() == FE_DOWNWARD;
    const bool flagsMatch = std::fetestexcept(FE_ALL_EXCEPT) == expectedFlags;
#if defined(__SSE2__)
    const bool controlMatches = _mm_getcsr() == expectedControl;
    _mm_setcsr(previousControl);
#else
    const bool controlMatches = true;
#endif
    const bool restored = std::fesetenv(&saved) == 0;
    return valuesMatch && roundingMatches && flagsMatch && controlMatches && restored;
}

template<class Query>
bool verifyResultComposition()
{
    using WordResult = decltype(Query::Scalar(1u, true, false));
    static_assert(noexcept(Query::Scalar(1u, true, false)));
    static_assert(noexcept(Query::Record(1.0f, true, false)));
    static_assert(noexcept(Query::Branch(std::declval<WordResult>(), 0u)));
    static_assert(noexcept(Query::Joined(std::declval<WordResult>(), false)));
    static_assert(noexcept(Query::ShortCircuit(std::declval<WordResult>(), 0u)));
    for (const auto word : {0u, 1u, 0x80000000u, 0xffffffffu}) {
        for (const bool enabled : {false, true}) {
            for (const bool cancelled : {false, true}) {
                const auto scalar = Query::Scalar(word, enabled, cancelled);
                const auto record = Query::Record(std::bit_cast<float>(word), enabled, cancelled);
                using Error = std::remove_cvref_t<decltype(scalar.error())>;
                const bool success = enabled && !cancelled;
                if (scalar.is_success() != success || record.is_success() != success ||
                    scalar.is_cancelled() != cancelled || record.is_cancelled() != cancelled ||
                    scalar.is_failure() != (!enabled && !cancelled) ||
                    record.is_failure() != (!enabled && !cancelled))
                    return false;
                const auto forwarded = Query::EarlyStatuses(record);
                if (forwarded.outcome() != record.outcome() || forwarded.error() != record.error())
                    return false;
                const auto expectedWord = static_cast<std::uint32_t>(word + 1u);
                if (success) {
                    if (scalar.value() != expectedWord || record.value().Word != expectedWord ||
                        std::bit_cast<std::uint32_t>(record.value().Value) != word ||
                        record.value().Error != Error::None || !record.value().Enabled ||
                        std::bit_cast<std::uint32_t>(forwarded.value().Value) != word ||
                        forwarded.value().Word != expectedWord)
                        return false;
                } else {
                    const auto expectedError = cancelled ? Error::Interrupted : Error::Invalid;
                    if (scalar.error() != expectedError || record.error() != expectedError)
                        return false;
                }
                const auto extracted = success ? expectedWord : 0u;
                if (Query::Branch(scalar, 0x55u) != (success ? expectedWord : 0x55u) ||
                    Query::NegatedStatuses(scalar) != extracted ||
                    Query::Joined(scalar, false) != extracted || Query::Joined(scalar, true) != extracted ||
                    Query::Conditional(scalar) != extracted || Query::GuardedHelper(scalar) != extracted ||
                    Query::AliasAfterGuard(scalar) != extracted ||
                    Query::ShortCircuit(scalar, expectedWord) != success ||
                    Query::ShortCircuit(scalar, expectedWord ^ 1u))
                    return false;
            }
        }
    }
    return true;
}

bool verifyNumericResults()
{
    if (!verifyResultComposition<numeric_fixture::native_numeric::Numeric::CompositionQueries>() ||
        !verifyResultComposition<numeric_compiled_fixture::native_numeric::Numeric::CompositionQueries>())
        return false;
    if (!verifyClosedNumericResults() || !verifyHostileNumericEnvironment())
        return false;
    return verifyResultProjection<numeric_fixture::native_numeric::Numeric::ResultQueries,
        numeric_fixture::source::Numeric::NumericResult>() &&
        verifyResultProjection<numeric_compiled_fixture::native_numeric::Numeric::ResultQueries,
        numeric_compiled_fixture::source::Numeric::NumericResult>();
}
)cpp";
}
