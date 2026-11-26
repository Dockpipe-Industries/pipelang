// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericHeaderFixture.hpp"

namespace {
using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command)
{
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "standalone numeric header failed (exit " + std::to_string(result.exit) + "): " + result.output + result.diagnostic);
}
}

void nativeNumericHeaderContracts(const std::filesystem::path& prefix,
                                  const std::filesystem::path& output)
{
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Queries.pipe", R"pipe(module Header;
public Enum NumericError { Invalid, Interrupted }
public Record Channels {
    public int First;
    public int Second;
    public NumericError Error = NumericError.Invalid;
}
public Record UnsafeDefaults {
    public int Value = List<int>.Empty().At(0);
}
public Record FloatingChannels {
    public float32 First;
    public float32 Second;
    public float32 Third;
    public float32 Fourth;
}
public Record IntegerChannels {
    public int First;
    public int Second;
    public int Third;
    public int Fourth;
}
public model Class Queries {
    private const float32 InverseMaximum = 1.0f / 65535.0f;
    public pure FloatingChannels NormalizeFour(int first, int second, int third, int fourth) {
        return new FloatingChannels {
            First = float32.FromInt(first) * InverseMaximum,
            Second = float32.FromInt(second) * InverseMaximum,
            Third = float32.FromInt(third) * InverseMaximum,
            Fourth = float32.FromInt(fourth) * InverseMaximum
        };
    }
    private pure int Quantize(float32 value) {
        if (!float32.IsFinite(value))
            return 0;
        return int.RoundFromFloat32(float32.Clamp(value, 0.0f, 1.0f) * 65535.0f);
    }
    public pure IntegerChannels QuantizeFour(float32 first, float32 second, float32 third, float32 fourth) {
        int firstWord = Quantize(first);
        int secondWord = Quantize(second);
        int thirdWord = Quantize(third);
        int fourthWord = Quantize(fourth);
        return new IntegerChannels {
            First = firstWord,
            Second = secondWord,
            Third = thirdWord,
            Fourth = fourthWord
        };
    }
    public pure float ClampWide(float value) {
        if (!float.IsFinite(value))
            return 0.0;
        return float.Clamp(value, -1.0, 1.0);
    }
    public pure float32 ClampSequenced(float32 value) {
        if (!float32.IsFinite(value))
            return 0.0f;
        return float32.Clamp(float32.Clamp(value, 0.0f, 1.0f),
            float32.Clamp(0.25f, 0.0f, 0.5f), float32.Clamp(0.75f, 0.5f, 1.0f));
    }
    public pure float32 UnprovedClamp(float32 value, float32 minimum, float32 maximum)
        => float32.Clamp(value, minimum, maximum);
    public pure float32 ReversedClamp() => float32.Clamp(0.0f, 1.0f, -1.0f);
    public pure int Expand(int value) {
        if (value < 0 || value > 255)
            return 0;
        return value * 257;
    }
    public pure int Narrow(int value) {
        if (value < 0 || value > 65535)
            return 0;
        return (value + 128) / 257;
    }
    public pure Channels Pack(int first, int second)
        => new Channels { First = Expand(first), Second = Expand(second) };
    public pure Result<int, NumericError> Checked(int value) {
        if (value < 0 || value > 255)
            return Result<int, NumericError>.Failure(NumericError.Invalid);
        return Result<int, NumericError>.Success(Expand(value));
    }
    public pure Result<int, NumericError> Forward(Result<int, NumericError> value) => value;
    public pure int Composed(int value) => Helpers.Project(value);
    public pure uint32 Wrap(uint32 value) => uint32.WrappingAdd(value, 1u);
    public pure Result<void, NumericError> Complete(bool enabled)
        => enabled ? Result<void, NumericError>.Success() : Result<void, NumericError>.Failure(NumericError.Invalid);
    public pure int ReadUnsafe(UnsafeDefaults value) => value.Value;
    public pure float32 Normalize(int value) => float32.FromInt(value) / 65535.0f;
    public pure float32 Third() => 1.0f / 3.0f;
    public pure float32 Product(float32 first, float32 second) => first * second;
    public pure float32 NarrowWide(float value) => float32.FromFloat(value);
}
public model Class Helpers {
    public pure int Project(int value) {
        if (value < 0 || value > 100)
            return 0;
        return value * 3;
    }
}
)pipe");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto checksum = fileHash(manifest);
    generateProjectBindings(prefix, manifest, checksum, compiler, output, "src", "Queries.pipe",
        "constant_header", output / "default");
    const auto information = compiledProjectInformation(prefix, manifest, checksum, compiler,
        output, "src", "Queries.pipe", "constant_compiled");
    generateCompiledProjectBindings(prefix, manifest, checksum, compiler, output, "src", "Queries.pipe",
        "constant_compiled", output / "compiled", information.at("plan_sha256").get<std::string>());
    generateProjectBindings(prefix, manifest, checksum, compiler, output, "src", "Queries.pipe",
        "caller_header", output / "active", "active-host");
    const auto activeInformation = compiledProjectInformation(prefix, manifest, checksum, compiler,
        output, "src", "Queries.pipe", "caller_compiled", "active-host");
    generateCompiledProjectBindings(prefix, manifest, checksum, compiler, output, "src", "Queries.pipe",
        "caller_compiled", output / "active-compiled", activeInformation.at("plan_sha256").get<std::string>(), "active-host");
    const auto header = readFile(output / "default/Numeric.hpp", 4U << 20);
    require(header.find("static constexpr std::int64_t Expand(") != std::string::npos,
        "proved integer closure lost constexpr emission");
    require(header.find("static constexpr float Third(") == std::string::npos,
        "floating closure acquired a constexpr promise");
    require(header.find("NarrowWide(") == std::string::npos,
        "caller-host header admitted unsupported floating narrowing");
    require(header.find("UnprovedClamp(") == std::string::npos && header.find("ReversedClamp(") == std::string::npos,
        "native header admitted unproved clamp preconditions");
    require(header.find("std::clamp(") != std::string::npos, "caller-host clamp lost native expression lowering");
    require(header.find("ReadUnsafe(") == std::string::npos,
        "standalone header admitted an ordinary-runtime record default");
    require(readFile(output / "default/Model.hpp", 4U << 20).find("ReadUnsafe(") != std::string::npos,
        "standalone admission changed ordinary numeric exports");
    for (const auto& forbidden : {"runtime.hpp", "Types.hpp", "Model.hpp", "TaskScope", "ObjectRef"})
        require(header.find(forbidden) == std::string::npos, "standalone numeric header includes an ordinary runtime dependency");
    atomicWrite(output / "standalone.cpp", nativeNumericHeaderConsumer() + R"cpp(
int main()
{
    return verifyClampFailures<float>() && verifyClampFailures<double>() && verifyNumericHeaders() ? 0 : 1;
}
)cpp");
    atomicWrite(output / "second.cpp", R"cpp(#include "../header-project/default/Numeric.hpp"
#include "../header-project/active/Numeric.hpp"
#include "../header-project/default/Model.hpp"
std::int64_t secondTranslationUnit() {
    return constant_header::numeric_header::native_numeric::Header::Queries::Expand(5);
}
)cpp");
    const auto executable = nativeExecutable(output / "standalone");
    std::vector<std::string> command{nativeTool("c++").string(), "-std=c++20", "-O2",
        "-Wall", "-Wextra", "-Werror", "-ffp-contract=fast", "-I" + (prefix / "include").string(),
        "-I" + output.string(), (output / "standalone.cpp").string(), (output / "second.cpp").string(),
        "-o", executable.string()};
#ifdef _WIN32
    // Match the installed Windows SDK and other numeric consumer fixtures.
    command.push_back("-static");
#endif
    invoke(command);
    invoke({executable.string()});
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"standalone_without_model_or_threads", true}, {"two_translation_units", true},
        {"header_and_compiled_constexpr", true}, {"caller_host_without_global_options", true}}));
}

std::string nativeNumericHeaderConsumer()
{
    return R"cpp(
#if defined(PIPELANG_ACTIVE_HOST_FLOATING)
#error "Standalone header fixture must begin without the active-host definition"
#endif
#include "../header-project/default/Numeric.hpp"
#include "../header-project/compiled/Numeric.hpp"
#include "../header-project/active/Numeric.hpp"
#include "../header-project/active-compiled/Numeric.hpp"
#if defined(PIPELANG_ACTIVE_HOST_FLOATING)
#error "Caller-host numeric header changed the floating definition"
#endif
#include <cfenv>
#include <cmath>
#include <algorithm>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
#include <bit>
#include <limits>

using ConstantHeader = constant_header::numeric_header::native_numeric::Header::Queries;
using ConstantCompiled = constant_compiled::numeric_header::native_numeric::Header::Queries;
using CallerHeader = caller_header::numeric_header::native_numeric::Header::Queries;
using CallerCompiled = caller_compiled::numeric_header::native_numeric::Header::Queries;
using ConstantError = constant_header::numeric_header::source::Header::NumericError;
using ConstantResult = pipelang::model::Result<std::int64_t, ConstantError>;
static_assert(ConstantHeader::Expand(255) == 65535);
static_assert(ConstantHeader::Wrap(UINT32_MAX) == 0u);
static_assert(ConstantHeader::Complete(true).is_success());
static_assert(ConstantHeader::Complete(false).error() == ConstantError::Invalid);
static_assert(ConstantHeader::Narrow(65535) == 255);
static_assert(ConstantHeader::Composed(7) == 21);
static_assert(ConstantHeader::Pack(3, 5).Second == 1285);
static_assert(ConstantHeader::Pack(3, 5).Error == ConstantError::Invalid);
static_assert(ConstantHeader::Checked(255).is_success());
static_assert(ConstantHeader::Checked(255).value() == 65535);
static_assert(ConstantHeader::Checked(256).is_failure());
static_assert(ConstantHeader::Forward(ConstantResult::Cancelled(ConstantError::Interrupted)).is_cancelled());
static_assert(ConstantHeader::Forward(ConstantResult::Cancelled(ConstantError::Interrupted)).error() == ConstantError::Interrupted);
static_assert(ConstantCompiled::Expand(255) == 65535);
static_assert(ConstantCompiled::Composed(9) == 27);
static_assert(CallerHeader::Expand(255) == 65535);
static_assert(CallerCompiled::Expand(255) == 65535);

template<class Query>
bool verifyCallerHostNumericHeader()
{
    std::fenv_t original;
    std::fegetenv(&original);
    bool correct = true;
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        std::fesetround(mode);
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile float expectedLiteral = 1.0f / 3.0f;
        const int expectedLiteralFlags = std::fetestexcept(FE_ALL_EXCEPT);
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile float actualLiteral = Query::Third();
        const int actualLiteralFlags = std::fetestexcept(FE_ALL_EXCEPT);
        correct = correct && std::bit_cast<std::uint32_t>(float(actualLiteral)) ==
            std::bit_cast<std::uint32_t>(float(expectedLiteral)) && actualLiteralFlags == expectedLiteralFlags;
        volatile std::int64_t integer = 10000;
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile float expected = static_cast<float>(integer) / 65535.0f;
        const int expectedFlags = std::fetestexcept(FE_ALL_EXCEPT);
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile float actual = Query::Normalize(integer);
        const int actualFlags = std::fetestexcept(FE_ALL_EXCEPT);
        correct = correct && std::bit_cast<std::uint32_t>(float(actual)) ==
            std::bit_cast<std::uint32_t>(float(expected)) && actualFlags == expectedFlags && std::fegetround() == mode;
    }
    std::fesetenv(&original);
    return correct;
}

struct PackedHeaderChannels {
    std::uint16_t first;
    std::uint16_t second;
    std::uint16_t third;
    std::uint16_t fourth;
};

std::array<float, 4> directHeaderChannels(PackedHeaderChannels value)
{
    const auto result = ConstantHeader::NormalizeFour(value.first, value.second, value.third, value.fourth);
    return {result.First, result.Second, result.Third, result.Fourth};
}

std::array<float, 4> referenceHeaderChannels(PackedHeaderChannels value)
{
    constexpr float inverseMaximum = 1.0f / 65535.0f;
    return {value.first * inverseMaximum, value.second * inverseMaximum,
        value.third * inverseMaximum, value.fourth * inverseMaximum};
}

[[gnu::noinline]] std::array<float, 4> directHeaderLiteral(unsigned sample)
{
    switch (sample) {
    case 0:
        return directHeaderChannels({0, 1, 32768, 65535});
    case 1:
        return directHeaderChannels({65535, 65535, 65535, 65535});
    default:
        return directHeaderChannels({123, 456, 789, 0});
    }
}

[[gnu::noinline]] std::array<float, 4> referenceHeaderLiteral(unsigned sample)
{
    switch (sample) {
    case 0:
        return referenceHeaderChannels({0, 1, 32768, 65535});
    case 1:
        return referenceHeaderChannels({65535, 65535, 65535, 65535});
    default:
        return referenceHeaderChannels({123, 456, 789, 0});
    }
}

[[gnu::noinline]] std::array<float, 4> directHeaderOpaque(const PackedHeaderChannels* value)
{
    return directHeaderChannels(*value);
}

[[gnu::noinline]] std::array<float, 4> referenceHeaderOpaque(const PackedHeaderChannels* value)
{
    return referenceHeaderChannels(*value);
}

std::array<std::uint16_t, 4> directHeaderInput(float first, float second, float third, float fourth)
{
    const auto result = ConstantHeader::QuantizeFour(first, second, third, fourth);
    return {static_cast<std::uint16_t>(result.First), static_cast<std::uint16_t>(result.Second),
        static_cast<std::uint16_t>(result.Third), static_cast<std::uint16_t>(result.Fourth)};
}

std::array<std::uint16_t, 4> referenceHeaderInput(float first, float second, float third, float fourth)
{
    const auto channel = [](float value) {
        if (!std::isfinite(value))
            return std::uint16_t{0};
        return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };
    return {channel(first), channel(second), channel(third), channel(fourth)};
}

[[gnu::noinline]] std::array<std::uint16_t, 4> directHeaderLiteralInput(unsigned sample)
{
    switch (sample) {
    case 0:
        return directHeaderInput(.25f, .5f, .75f, 1.f);
    case 1:
        return directHeaderInput(-.25f, 1.25f, 0.f, .47f);
    case 2:
        return directHeaderInput(std::bit_cast<float>(0x7f812345u), 1.f, .5f, -0.f);
    default:
        return directHeaderInput(std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), std::bit_cast<float>(1u), .125f);
    }
}

[[gnu::noinline]] std::array<std::uint16_t, 4> referenceHeaderLiteralInput(unsigned sample)
{
    switch (sample) {
    case 0:
        return referenceHeaderInput(.25f, .5f, .75f, 1.f);
    case 1:
        return referenceHeaderInput(-.25f, 1.25f, 0.f, .47f);
    case 2:
        return referenceHeaderInput(std::bit_cast<float>(0x7f812345u), 1.f, .5f, -0.f);
    default:
        return referenceHeaderInput(std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), std::bit_cast<float>(1u), .125f);
    }
}

template<class Actual, class Expected>
bool sameHeaderFloatingCall(Actual actual, Expected expected)
{
    std::fenv_t initial;
    std::fegetenv(&initial);
#if defined(__SSE2__)
    const unsigned initialControl = _mm_getcsr();
#endif
    const auto expectedValues = expected();
    const int expectedFlags = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__SSE2__)
    const unsigned expectedControl = _mm_getcsr();
#endif
    std::fesetenv(&initial);
#if defined(__SSE2__)
    _mm_setcsr(initialControl);
#endif
    const auto actualValues = actual();
    const int actualFlags = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__SSE2__)
    if (_mm_getcsr() != expectedControl)
        return false;
#endif
    if (actualFlags != expectedFlags)
        return false;
    return std::bit_cast<std::array<std::byte, sizeof(actualValues)>>(actualValues) ==
        std::bit_cast<std::array<std::byte, sizeof(expectedValues)>>(expectedValues);
}

bool verifyDirectHeaderRecords()
{
    std::fenv_t original;
    std::fegetenv(&original);
#if defined(__SSE2__)
    const unsigned originalControl = _mm_getcsr();
#endif
    bool valid = true;
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        std::fesetround(rounding);
#if defined(__SSE2__)
        for (const unsigned nativeRounding : {0u, 0x2000u, 0x4000u, 0x6000u}) {
            for (const unsigned denormals : {0u, 0x40u, 0x8000u, 0x8040u}) {
                _mm_setcsr((_mm_getcsr() & ~0xe040u) | nativeRounding | denormals);
#endif
                for (unsigned sample = 0; sample < 4; ++sample) {
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::feraiseexcept(FE_DIVBYZERO);
                    valid = sameHeaderFloatingCall([&] { return directHeaderLiteral(sample); },
                        [&] { return referenceHeaderLiteral(sample); }) && valid;
                    valid = sameHeaderFloatingCall([&] { return directHeaderLiteralInput(sample); },
                        [&] { return referenceHeaderLiteralInput(sample); }) && valid;
                }
                const PackedHeaderChannels values{123, 456, 789, 65535};
                valid = sameHeaderFloatingCall([&] { return directHeaderOpaque(&values); },
                    [&] { return referenceHeaderOpaque(&values); }) && valid;
#if defined(__SSE2__)
            }
        }
#endif
    }
    std::fesetenv(&original);
#if defined(__SSE2__)
    _mm_setcsr(originalControl);
#endif
    return valid;
}

template<class Float>
bool verifyClampFailures()
{
    const Float infinity = std::numeric_limits<Float>::infinity();
    const Float nan = std::numeric_limits<Float>::quiet_NaN();
    const std::array<std::array<Float, 3>, 7> invalid{{
        {Float(0), Float(1), Float(-1)}, {infinity, Float(0), Float(1)},
        {nan, Float(0), Float(1)}, {Float(0), -infinity, Float(1)},
        {Float(0), Float(0), infinity}, {Float(0), nan, Float(1)}, {Float(0), Float(0), nan}
    }};
    for (const auto& values : invalid) {
        try {
            (void)pipelang::model::clampFinite(values[0], values[1], values[2]);
            return false;
        } catch (const pipelang::model::ExecutionFailure&) {
        }
    }
    return pipelang::model::clampFinite(Float(2), Float(-1), Float(1)) == Float(1);
}

bool verifyNumericHeaders()
{
    if (ConstantHeader::ClampWide(-2.0) != -1.0 || ConstantCompiled::ClampWide(2.0) != 1.0 ||
        ConstantHeader::ClampSequenced(-1.0f) != 0.25f || ConstantCompiled::ClampSequenced(2.0f) != 0.75f)
        return false;
    for (std::int64_t value = 0; value <= 65535; ++value) {
        const auto expected = (value + 128) / 257;
        if (ConstantHeader::Narrow(value) != expected || ConstantCompiled::Narrow(value) != expected)
            return false;
    }
    return verifyCallerHostNumericHeader<CallerHeader>() && verifyCallerHostNumericHeader<CallerCompiled>() &&
        verifyDirectHeaderRecords();
}
)cpp";
}
