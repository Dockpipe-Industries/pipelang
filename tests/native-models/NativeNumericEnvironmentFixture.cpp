// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericEnvironmentFixture.hpp"

#include <set>

void writeNativeNumericEnvironmentFixture(const std::filesystem::path& output)
{
    pipelang::tools::atomicWrite(output / "src/NumericEnvironment.pipe", R"pipe(module Numeric;
public Enum NumericEnvironmentError { Invalid, Interrupted }
public Record WideEnvironmentValue {
    public float Origin;
    public float Endpoint;
    public bool Valid;
}
public Record MixedEnvironmentValue {
    public float Wide;
    public float32 Single;
    public bool Valid;
}
public Record WideEnvironmentDefault {
    public float Value = float.FromFloat32(1.0f);
}
public model Class WideEnvironmentMath {
    public pure float Add(float origin, float extent) => origin + extent;
}
public model Class MixedEnvironmentMath {
    public pure float Read(float value) {
        float32 unused = 0.0f;
        return value;
    }
}
public model Class EnvironmentQueries {
    public pure float Add(float origin, float extent) => WideEnvironmentMath.Add(origin, extent);
    public pure bool WideComparisonOr(float first, float second, float left, float right)
        => first == second || left == right;
    public pure bool WideComparisonAnd(float first, float second, float left, float right)
        => first == second && left == right;
    public pure bool SingleComparisonOr(float32 first, float32 second, float32 left, float32 right)
        => first == second || left == right;
    public pure bool SingleComparisonAnd(float32 first, float32 second, float32 left, float32 right)
        => first == second && left == right;
    public pure Result<WideEnvironmentValue, NumericEnvironmentError> Build(float origin, float extent) {
        if (!float.IsFinite(origin) || !float.IsFinite(extent))
            return Result<WideEnvironmentValue, NumericEnvironmentError>.Failure(NumericEnvironmentError.Invalid);
        return Result<WideEnvironmentValue, NumericEnvironmentError>.Success(new WideEnvironmentValue {
            Origin = origin, Endpoint = Add(origin, extent), Valid = true
        });
    }
    public pure Result<bool, NumericEnvironmentError> Nonnegative(float value) {
        if (!float.IsFinite(value))
            return Result<bool, NumericEnvironmentError>.Failure(NumericEnvironmentError.Invalid);
        return Result<bool, NumericEnvironmentError>.Success(value >= 0.0);
    }
    public pure Result<WideEnvironmentValue, NumericEnvironmentError> Forward(
        Result<WideEnvironmentValue, NumericEnvironmentError> value) => value;
    public pure Result<float, NumericEnvironmentError> Extract(Result<WideEnvironmentValue, NumericEnvironmentError> value) {
        if (value.IsCancelled)
            return Result<float, NumericEnvironmentError>.Cancelled(value.Error);
        if (!value.IsSuccess)
            return Result<float, NumericEnvironmentError>.Failure(value.Error);
        return Result<float, NumericEnvironmentError>.Success(value.Value.Endpoint);
    }
    public pure Result<WideEnvironmentValue, NumericEnvironmentError> Cancel()
        => Result<WideEnvironmentValue, NumericEnvironmentError>.Cancelled(NumericEnvironmentError.Interrupted);
    public pure Result<WideEnvironmentValue, NumericEnvironmentError> Reject()
        => Result<WideEnvironmentValue, NumericEnvironmentError>.Failure(NumericEnvironmentError.Invalid);
    public pure int EchoInteger(int value) => value;
    public pure uint32 Mask(uint32 value) => value & 0xffu;
    public pure NumericEnvironmentError EchoError(NumericEnvironmentError value) => value;
    public pure bool EchoBoolean(bool value) => value;
    public pure float UnrelatedSingleMethod(float value) => value;
    public pure bool ZeroTripWideCondition() {
        int index = 0;
        while (index < (float.IsFinite(0.0) ? 0 : 0)) {
            index = index + 1;
        }
        return true;
    }

    private pure bool SinglePredicate() {
        float32 unused = 0.0f;
        return true;
    }
    public pure bool ZeroTripSingleCondition() {
        int index = 0;
        while (index < (SinglePredicate() ? 0 : 0)) {
            index = index + 1;
        }
        return true;
    }
    private pure bool UnsafePredicate() => List<bool>.Empty().At(0);
    public pure bool UnsafeZeroTripCondition() {
        int index = 0;
        while (index < (UnsafePredicate() ? 0 : 0)) {
            index = index + 1;
        }
        return true;
    }

    public pure float32 Single(float32 value) => value;
    public pure float MixedConversion(float value) => float.FromFloat32(float32.FromFloat(value));
    private pure float HiddenSingleLocal(float value) {
        float32 unused = 1.0f;
        return value;
    }
    public pure float ViaSingleLocal(float value) => HiddenSingleLocal(value);
    public pure float ViaQualifiedSingle(float value) => MixedEnvironmentMath.Read(value);
    public pure bool MixedParameter(MixedEnvironmentValue value) => value.Valid;
    public pure MixedEnvironmentValue MixedResult(float value)
        => new MixedEnvironmentValue { Wide = value, Valid = true };
    public pure bool EmptySingleArray() {
        Array<float32> values = Array<float32>.Create(0, 0.0f);
        return true;
    }
    public pure float HiddenDefaultSingle() {
        WideEnvironmentDefault defaults = new WideEnvironmentDefault { };
        return defaults.Value;
    }
}
)pipe");
}

void verifyNativeNumericEnvironmentExports(const pipelang::tools::Json& manifest)
{
    using namespace pipelang::tools;
    const std::set<std::string> independent{
        "Add", "Build", "Nonnegative", "Forward", "Extract", "Cancel", "Reject",
        "EchoInteger", "Mask", "EchoError", "EchoBoolean", "UnrelatedSingleMethod", "ZeroTripWideCondition",
        "WideComparisonOr", "WideComparisonAnd"};
    const std::set<std::string> dependent{
        "Single", "MixedConversion", "ViaSingleLocal", "ViaQualifiedSingle",
        "MixedParameter", "MixedResult", "EmptySingleArray", "HiddenDefaultSingle", "ZeroTripSingleCondition",
        "SingleComparisonOr", "SingleComparisonAnd"};
    std::set<std::string> observed;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        const auto owner = item.at("owner").get<std::string>();
        if (owner == "Numeric.WideEnvironmentMath" || owner == "Numeric.MixedEnvironmentMath") {
            require(item.at("binary32Environment") ==
                (owner == "Numeric.WideEnvironmentMath" ? "not-required" : "caller-admitted"),
                "qualified helper environment proof changed");
            continue;
        }
        if (owner != "Numeric.EnvironmentQueries")
            continue;
        const auto method = item.at("method").get<std::string>();
        require(independent.contains(method) || dependent.contains(method), "unexpected environment fixture export");
        const auto expected = independent.contains(method) ? "not-required" : "caller-admitted";
        require(item.at("binary32Environment") == expected && item.at("allocationFree") == true &&
            item.at("noexcept") == true, "numeric closure environment metadata mismatch: " + method);
        observed.insert(method);
    }
    auto expected = independent;
    expected.insert(dependent.begin(), dependent.end());
    require(observed == expected, "numeric closure environment fixture lost a safe method");
}

std::string nativeNumericEnvironmentConsumer()
{
    return R"cpp(
struct NumericEnvironmentRestore {
    std::fenv_t environment{};
    bool captured = false;
#if defined(__SSE__) || defined(_M_X64)
    unsigned control = 0;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t control = 0;
    std::uint64_t status = 0;
#endif
    NumericEnvironmentRestore() noexcept
        : captured(std::fegetenv(&environment) == 0)
    {
#if defined(__SSE__) || defined(_M_X64)
        control = _mm_getcsr();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ volatile("mrs %0, fpcr" : "=r"(control) : : "memory");
        __asm__ volatile("mrs %0, fpsr" : "=r"(status) : : "memory");
#endif
    }
    bool restore() const noexcept
    {
        if (!captured || std::fesetenv(&environment) != 0)
            return false;
#if defined(__SSE__) || defined(_M_X64)
        _mm_setcsr(control);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ volatile("msr fpcr, %0\nmsr fpsr, %1" : : "r"(control), "r"(status) : "memory");
#endif
        return true;
    }
    ~NumericEnvironmentRestore() noexcept
    {
        if (captured && !restore())
            std::abort();
    }
};

std::uint64_t numericEnvironmentControls() noexcept
{
#if defined(__SSE__) || defined(_M_X64)
    return _mm_getcsr() & ~UINT64_C(0x3f);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t control;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control) : : "memory");
    return control;
#else
    return 0;
#endif
}

std::uint64_t numericComparisonStatus() noexcept
{
#if defined(__SSE__) || defined(_M_X64)
    return _mm_getcsr() & 0x3fU;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t status;
    __asm__ volatile("mrs %0, fpsr" : "=r"(status) : : "memory");
    return status;
#else
    return 0;
#endif
}

struct NumericComparisonEnvironment {
    int rounding;
    int exceptions;
    std::uint64_t controls;
    std::uint64_t status;

    static NumericComparisonEnvironment capture() noexcept
    {
        return {std::fegetround(), std::fetestexcept(FE_ALL_EXCEPT),
            numericEnvironmentControls(), numericComparisonStatus()};
    }
    bool operator==(const NumericComparisonEnvironment&) const = default;
};

bool prepareNumericComparison(std::size_t seed)
{
    const std::array portableSeeds{0, FE_INEXACT, FE_INVALID | FE_DIVBYZERO};
    if (std::feclearexcept(FE_ALL_EXCEPT) != 0 || std::feraiseexcept(portableSeeds[seed]) != 0)
        return false;
#if defined(__SSE__) || defined(_M_X64)
    const std::array<unsigned, 3> rawSeeds{0U, 0x20U, 0x05U};
    _mm_setcsr((_mm_getcsr() & ~0x3fU) | rawSeeds[seed]);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    const std::array<std::uint64_t, 3> rawSeeds{0, 0x10, 0x03};
    __asm__ volatile("msr fpsr, %0" : : "r"(rawSeeds[seed]) : "memory");
#endif
    return true;
}

template<class Number>
bool referenceNumericEquality(Number left, Number right) noexcept
{
    return left == right;
}

template<class Number, class Word, std::size_t Count>
bool verifyNumericComparisonProjection(
    bool (*orQuery)(Number, Number, Number, Number) noexcept,
    bool (*andQuery)(Number, Number, Number, Number) noexcept,
    const std::array<std::array<Word, 2>, Count>& inputs)
{
    using Query = bool (*)(Number, Number, Number, Number) noexcept;
    using Reference = bool (*)(Number, Number) noexcept;
    // Indirect calls through volatile pointers keep runtime arguments and native
    // call boundaries even for header-only exports. The reference performs one
    // comparison only; it cannot speculate a second side of a boolean expression.
    Reference volatile reference = &referenceNumericEquality<Number>;
    for (const bool useOr : {false, true}) {
        Query volatile query = useOr ? orQuery : andQuery;
        for (const bool firstEqual : {false, true}) {
            const Number first = Number(1);
            const Number second = firstEqual ? Number(1) : Number(2);
            const bool skipRight = useOr ? firstEqual : !firstEqual;
            for (const auto& input : inputs) {
                const Number left = std::bit_cast<Number>(input[0]);
                const Number right = std::bit_cast<Number>(input[1]);
                for (std::size_t seed = 0; seed < 3; ++seed) {
                    if (!prepareNumericComparison(seed))
                        return false;
                    NumericEnvironmentRestore initial;
                    if (!initial.captured)
                        return false;
                    bool expected = firstEqual;
                    if (!skipRight)
                        expected = reference(left, right);
                    const auto expectedEnvironment = NumericComparisonEnvironment::capture();
                    if (!initial.restore())
                        return false;
                    const bool actual = query(first, second, left, right);
                    const auto actualEnvironment = NumericComparisonEnvironment::capture();
                    if (actual != expected || actualEnvironment != expectedEnvironment) {
                        std::fprintf(stderr,
                            "floating comparison width=%zu operator=%s first-equal=%d seed=%zu "
                            "left=%016llx right=%016llx result=%d/%d "
                            "rounding=%x/%x exceptions=%x/%x controls=%llx/%llx status=%llx/%llx\n",
                            sizeof(Number) * 8, useOr ? "||" : "&&", int(firstEqual), seed,
                            static_cast<unsigned long long>(input[0]),
                            static_cast<unsigned long long>(input[1]), int(actual), int(expected),
                            unsigned(actualEnvironment.rounding), unsigned(expectedEnvironment.rounding),
                            unsigned(actualEnvironment.exceptions), unsigned(expectedEnvironment.exceptions),
                            static_cast<unsigned long long>(actualEnvironment.controls),
                            static_cast<unsigned long long>(expectedEnvironment.controls),
                            static_cast<unsigned long long>(actualEnvironment.status),
                            static_cast<unsigned long long>(expectedEnvironment.status));
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

template<class Query>
bool verifyWideComparisonProjection()
{
    const std::array<std::array<std::uint64_t, 2>, 8> inputs{{
        {UINT64_C(0), UINT64_C(0x8000000000000000)},
        {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)},
        {UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000)},
        {UINT64_C(1), UINT64_C(0)},
        {UINT64_C(0x8000000000000001), UINT64_C(1)},
        {UINT64_C(0x7ff0000000000001), UINT64_C(0)},
        {UINT64_C(0), UINT64_C(0xfff0000000000001)},
        {UINT64_C(0x7ff8000000000001), UINT64_C(0)}
    }};
    return verifyNumericComparisonProjection(&Query::WideComparisonOr, &Query::WideComparisonAnd, inputs);
}

template<class Query>
bool verifySingleComparisonProjection()
{
    const std::array<std::array<std::uint32_t, 2>, 8> inputs{{
        {0U, 0x80000000U},
        {0x3f800000U, 0x3f800000U},
        {0x3f800000U, 0x40000000U},
        {1U, 0U},
        {0x80000001U, 1U},
        {0x7f800001U, 0U},
        {0U, 0xff800001U},
        {0x7fc00001U, 0U}
    }};
    return verifyNumericComparisonProjection(&Query::SingleComparisonOr, &Query::SingleComparisonAnd, inputs);
}

template<class Query>
bool verifyWideEnvironmentProjection()
{
    static_assert(noexcept(Query::Add(0.0, 0.0)));
    static_assert(noexcept(Query::Build(0.0, 0.0)));
    static_assert(noexcept(Query::Forward(Query::Cancel())));
    static_assert(noexcept(Query::Extract(Query::Reject())));
    static_assert(noexcept(Query::Nonnegative(0.0)));
    static_assert(noexcept(Query::EchoInteger(0)));
    static_assert(noexcept(Query::EchoBoolean(true)));
    static_assert(noexcept(Query::Mask(0u)));
    static_assert(noexcept(Query::ZeroTripWideCondition()));
    const auto controls = numericEnvironmentControls();
    const auto rounding = std::fegetround();
    // Volatile input storage and an independently rounded binary64 sum prevent
    // constants or a generated helper from defining the expected answer.
    for (const auto bits : {UINT64_C(0), UINT64_C(0x8000000000000000), UINT64_C(1),
                           UINT64_C(0x0010000000000000), UINT64_C(0x3ff0000000000000)}) {
        volatile double origin = std::bit_cast<double>(bits);
        volatile double extent = 0x1p-53;
        volatile double sum = origin + extent;
        const double first = origin;
        const double second = extent;
        const auto expected = std::bit_cast<std::uint64_t>(double(sum));
        const auto actual = Query::Build(first, second);
        if (!actual.is_success() || !actual.value().Valid ||
            std::bit_cast<std::uint64_t>(actual.value().Origin) != bits ||
            std::bit_cast<std::uint64_t>(actual.value().Endpoint) != expected ||
            std::bit_cast<std::uint64_t>(Query::Add(first, second)) != expected)
            return false;
        const auto forwarded = Query::Forward(actual);
        const auto extracted = Query::Extract(forwarded);
        if (!forwarded.is_success() || !extracted.is_success() ||
            std::bit_cast<std::uint64_t>(extracted.value()) != expected)
            return false;
    }
    volatile double subnormal = std::bit_cast<double>(UINT64_C(1));
    volatile double subnormalSum = subnormal + subnormal;
    if (std::bit_cast<std::uint64_t>(Query::Add(subnormal, subnormal)) !=
        std::bit_cast<std::uint64_t>(double(subnormalSum)))
        return false;
    const auto rejected = Query::Forward(Query::Reject());
    const auto cancelled = Query::Forward(Query::Cancel());
    const auto failedValue = Query::Extract(rejected);
    const auto cancelledValue = Query::Extract(cancelled);
    if (!rejected.is_failure() || !cancelled.is_cancelled() || !failedValue.is_failure() ||
        !cancelledValue.is_cancelled() || failedValue.error() != rejected.error() ||
        cancelledValue.error() != cancelled.error() || Query::EchoError(rejected.error()) != rejected.error())
        return false;
    const auto invalid = Query::Build(std::numeric_limits<double>::infinity(), 0.0);
    const auto negative = Query::Nonnegative(-1.0);
    const auto positive = Query::Nonnegative(1.0);
    if (!invalid.is_failure() || invalid.error() != rejected.error() ||
        !negative.is_success() || negative.value() || !positive.is_success() || !positive.value() ||
        Query::EchoInteger(std::numeric_limits<std::int64_t>::min()) != std::numeric_limits<std::int64_t>::min() ||
        !Query::EchoBoolean(true) || Query::Mask(0x1234u) != 0x34u ||
        Query::UnrelatedSingleMethod(-0.0) != 0.0 || !Query::ZeroTripWideCondition())
        return false;
    return numericEnvironmentControls() == controls && std::fegetround() == rounding;
}

bool verifyNumericEnvironment()
{
    NumericEnvironmentRestore restore;
    if (!restore.captured)
        return false;
    std::fenv_t discarded;
    if (std::feholdexcept(&discarded) != 0)
        return false;
    // The caller admitted binary32 before this fixture. Keep these calls outside
    // the hostile-mode loop: their metadata still requires nearest and gradual.
    if (!verifySingleComparisonProjection<numeric_fixture::native_numeric::Numeric::EnvironmentQueries>() ||
        !verifySingleComparisonProjection<numeric_compiled_fixture::native_numeric::Numeric::EnvironmentQueries>())
        return false;
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
#if defined(__SSE__) || defined(_M_X64)
        for (unsigned hardware = 0; hardware < 4; ++hardware) {
            for (const unsigned denormals : {0u, 0x0040u, 0x8000u, 0x8040u}) {
#else
        for (unsigned hardware = 0; hardware < 1; ++hardware) {
            for (const unsigned denormals : {0u, 0x01000000u}) {
#endif
                if (std::fesetround(rounding) != 0)
                    return false;
#if defined(__SSE__) || defined(_M_X64)
                const auto control = _mm_getcsr();
                _mm_setcsr((control & ~0xe040u) | (hardware << 13) | denormals);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
                static_cast<void>(hardware);
                std::uint64_t control;
                __asm__ volatile("mrs %0, fpcr" : "=r"(control) : : "memory");
                control = (control & ~UINT64_C(0x01000000)) | denormals;
                __asm__ volatile("msr fpcr, %0" : : "r"(control) : "memory");
#else
                static_cast<void>(hardware);
                static_cast<void>(denormals);
#endif
                if (!verifyWideEnvironmentProjection<numeric_fixture::native_numeric::Numeric::EnvironmentQueries>() ||
                    !verifyWideEnvironmentProjection<numeric_compiled_fixture::native_numeric::Numeric::EnvironmentQueries>() ||
                    !verifyWideComparisonProjection<numeric_fixture::native_numeric::Numeric::EnvironmentQueries>() ||
                    !verifyWideComparisonProjection<numeric_compiled_fixture::native_numeric::Numeric::EnvironmentQueries>())
                    return false;
            }
        }
    }
    return true;
}
)cpp";
}
