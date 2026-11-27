// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeActiveNumericFixture.hpp"
#include "NativeExponentFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"

#include <set>

namespace {
using namespace pipelang::tools;

Json compileActive(const fs::path& prefix, const Json& request)
{
    const auto result = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
        request.dump(), std::chrono::seconds(45), 4U << 20);
    require(result.exit == 0, "active numeric compiler transport failed: " + result.output + result.diagnostic);
    const auto response = parseJson(result.output, 4U << 20);
    require(response.at("ok") == true, "active numeric source failed: " + response.dump());
    return response;
}
}

void nativeActiveNumericContracts(const std::filesystem::path& prefix,
                                  const std::filesystem::path& output)
{
    require(!fs::exists(output), "active numeric fixture must be fresh");
    fs::create_directories(output / "src");
    writeNativeExponentChecks(output);
    atomicWrite(output / "src/Queries.pipe", R"pipe(module Active;
public Enum NumericError { Invalid, Interrupted }
public Record Channels { public int First; public int Second; public int Third; }
public Record ComputedDefault { public float32 Value = 1.0f / 3.0f; }
public Record ComputedInteger { public int Value = 1 + 2; }
public model Class Queries {
    private const float32 OneThird = 1.0f / 3.0f;
    private const float32 TwoThirds = 2.0f / 3.0f;
    public pure float32 ConstantOneThird() => OneThird;
    public pure float32 ConstantTwoThirds() => TwoThirds;
    public pure float32 QualifiedAddThird(float32 value) => ScalarHelpers.Add(value, OneThird);
    public pure float32 QualifiedNormalize(int value) => ScalarHelpers.Normalize(value);
    public pure Result<int, NumericError> QualifiedNearest(float32 value) => ScalarHelpers.Nearest(value);
    public ComputedInteger ObserveRecord() {
        uint32 environment = FloatingEnvironment.Observe();
        return new ComputedInteger { };
    }
    public pure float32 SingleFromInteger(int value) => float32.FromInt(value);
    public pure float WideFromInteger(int value) => float.FromInt(value);
    public pure float32 Normalize(int value) => float32.FromInt(value) / 65535.0f;
    public pure float32 Add(float32 first, float32 second) => first + second;
    public pure float32 Divide(float32 first, float32 second) => first / second;
    public pure float32 Multiply(float32 first, float32 second) => first * second;
    public pure float32 LiteralDivide() => 1.0f / 3.0f;
    public pure float LiteralFloor() => float.Floor(1.5);
    public pure float32 SquareRootSingle(float32 value) => float32.Sqrt(value);
    public pure float SquareRootWide(float value) => float.Sqrt(value);
    public pure float32 LiteralSquareRootSingle() => float32.Sqrt(2.0f);
    public pure float LiteralSquareRootWide() => float.Sqrt(2.0);
    public pure float WideAdd(float first, float second) => first + second;
    public pure float Widen(float32 value) => float.FromFloat32(value);
    public pure Result<int, NumericError> Truncate(float32 value) {
        if (!float32.IsFinite(value) || value < -65535.0f || value > 65535.0f)
            return Result<int, NumericError>.Failure(NumericError.Invalid);
        return Result<int, NumericError>.Success(int.FromFloat32(value));
    }
    public pure Result<int, NumericError> Nearest(float32 value) {
        float rounded = float.Floor(float.FromFloat32(value) + 0.5);
        if (!float.IsFinite(rounded) || rounded < 0.0 || rounded > 65535.0)
            return Result<int, NumericError>.Failure(NumericError.Invalid);
        return Result<int, NumericError>.Success(int.FromFloat(rounded));
    }
    public pure Result<Channels, NumericError> Forward(Result<Channels, NumericError> value) => value;
    public pure float32 ComputedDefaultValue() {
        ComputedDefault value = new ComputedDefault { };
        return value.Value;
    }
    public pure float32 Narrow(float value) => float32.FromFloat(value);
    public pure int RoundSingle(float32 value) {
        if (!float32.IsFinite(value) || value < -65535.0f || value > 65535.0f)
            return -1;
        return int.RoundFromFloat32(value);
    }
    public pure int RoundWide(float value) {
        if (!float.IsFinite(value) || value < -65535.0 || value > 65535.0)
            return -1;
        return int.RoundFromFloat(value);
    }
    public pure int RoundProduct(float32 value, float32 scale) {
        if (!float32.IsFinite(value) || value < -1.0f || value > 1.0f ||
            !float32.IsFinite(scale) || scale < -65535.0f || scale > 65535.0f)
            return -1;
        float32 product = value * scale;
        return int.RoundFromFloat32(product);
    }
    public pure int OverflowProduct(float32 value) {
        if (!float32.IsFinite(value) || value < 0.0f || value > 3.4028234663852886e38f)
            return 0;
        return int.RoundFromFloat32(value * value);
    }
    public pure int NaNProduct(float32 value) {
        if (value < 0.0f || value > 1.0f)
            return 0;
        return int.RoundFromFloat32(value * 65535.0f);
    }
    public pure int UnsafeProduct(float32 value, float32 scale) => int.RoundFromFloat32(value * scale);
    public pure int LiteralRoundSingle() => int.RoundFromFloat32(1.401298464324817e-45f);
    public pure int LiteralRoundWide() => int.RoundFromFloat(4.9406564584124654e-324);
    public pure int UnsafeRound(float value) => int.RoundFromFloat(value);
    public pure int WideExponent(float value) => float.ILogB(value);
    public pure int SingleExponent(float32 value) => float32.ILogB(value);
    public pure float WideScale(float value, int exponent) => float.ScaleB(value, exponent);
    public pure float32 SingleScale(float32 value, int exponent) => float32.ScaleB(value, exponent);
    public pure float WideRemainder(float value, float other) => float.Fmod(value, other);
    public pure float32 SingleRemainder(float32 value, float32 other) => float32.Fmod(value, other);
    public pure float LiteralScale() => float.ScaleB(1.0, -1075);
    public pure float LiteralRemainder() => float.Fmod(5.5, 2.0);
    public pure float32 LiteralNarrow() => float32.FromFloat(1.000000059604644775390625);
    public pure int ExponentStep(float value) {
        int exponent = float.ILogB(value);
        int step = exponent - 10;
        if (step < -24)
            step = -24;
        return -step;
    }
    public pure float32 Transcendental(float32 value) => float32.Sin(value);
    public pure float32 SinSingle(float32 value) => float32.Sin(value);
    public pure float32 CosSingle(float32 value) => float32.Cos(value);
    public pure float SinWide(float value) => float.Sin(value);
    public pure float CosWide(float value) => float.Cos(value);
    public pure float32 LiteralSinSingle() => float32.Sin(1.0f);
    public pure float32 LiteralCosSingle() => float32.Cos(1.0f);
    public pure float LiteralSinWide() => float.Sin(1.0);
    public pure float LiteralCosWide() => float.Cos(1.0);
    public pure int Unsafe(float32 value) => int.FromFloat32(value);
}
)pipe");
    atomicWrite(output / "src/ScalarHelpers.pipe", R"pipe(module Active;
public model Class ScalarHelpers {
    public pure float32 Add(float32 first, float32 second) => first + second;
    public pure float32 Normalize(int value) => float32.FromInt(value) / 65535.0f;
    public pure Result<int, NumericError> Nearest(float32 value) {
        float rounded = float.Floor(float.FromFloat32(value) + 0.5);
        if (!float.IsFinite(rounded) || rounded < 0.0 || rounded > 65535.0)
            return Result<int, NumericError>.Failure(NumericError.Invalid);
        return Result<int, NumericError>.Success(int.FromFloat(rounded));
    }
}
)pipe");
    const auto project = SourceProject::load(output, "src", "Queries.pipe");
    const auto request = project.request("active_header");
    const auto ordinary = compileActive(prefix, request);
    require(!ordinary.at("manifest").contains("nativeNumericEnvironment"), "default profile acquired selection");
    for (const auto& item : ordinary.at("manifest").at("nativeNumericExports")) {
        require(item.at("method") != "SingleFromInteger" && item.at("method") != "WideFromInteger" &&
                item.at("method") != "Normalize" && item.at("method") != "QualifiedNormalize",
                "default integer conversion admission changed");
        require(item.at("binary32Environment") != "active-host", "default export lost its environment contract");
    }
    auto selected = request;
    selected["native_numeric_environment"] = "active-host";
    const auto response = compileActive(prefix, selected);
    const auto& model = response.at("manifest");
    require(model.at("nativeObservationExports") == ordinary.at("manifest").at("nativeObservationExports"),
        "active numeric selection changed independent observation admission");
    require(model.at("nativeNumericEnvironment") == "active-host", "active selection missing from manifest");
    std::set<std::string> methods;
    for (const auto& item : model.at("nativeNumericExports")) {
        const auto name = item.at("method").get<std::string>();
        methods.insert(name);
        const auto expected = name == "WideFromInteger" || name == "WideAdd" || name == "Forward" || name == "LiteralFloor"
            || name == "LiteralRoundWide" || name == "RoundWide" || name == "WideExponent" || name == "WideScale" || name == "WideRemainder"
            || name == "LiteralScale" || name == "LiteralRemainder" || name == "ExponentStep"
            || name == "SquareRootWide" || name == "LiteralSquareRootWide"
            || name == "SinWide" || name == "CosWide" || name == "LiteralSinWide" || name == "LiteralCosWide"
            ? "not-required" : "active-host";
        require(item.at("binary32Environment") == expected, "active environment proof mismatch: " + name);
    }
    require(methods == std::set<std::string>{"SingleFromInteger", "WideFromInteger", "Normalize", "Add",
        "Divide", "Multiply", "LiteralDivide", "LiteralFloor", "WideAdd", "Widen", "Truncate", "Nearest", "Forward",
        "ConstantOneThird", "ConstantTwoThirds", "QualifiedAddThird", "QualifiedNormalize", "QualifiedNearest",
        "Narrow", "WideExponent", "SingleExponent", "WideScale", "SingleScale", "WideRemainder",
        "SingleRemainder", "LiteralScale", "LiteralRemainder", "LiteralNarrow", "ExponentStep", "RoundWide", "RoundSingle", "RoundProduct", "LiteralRoundSingle", "LiteralRoundWide",
        "SquareRootSingle", "SquareRootWide", "LiteralSquareRootSingle", "LiteralSquareRootWide",
        "Transcendental", "SinSingle", "CosSingle", "SinWide", "CosWide", "LiteralSinSingle", "LiteralCosSingle",
        "LiteralSinWide", "LiteralCosWide"},
        "active projection admitted unsupported operations or lost safe ones");
    admitModel(model, "active_header");
    for (const Json& invalidSelection : {Json("nearest"), Json(false), Json(nullptr), Json::object()}) {
        auto invalid = request;
        invalid["native_numeric_environment"] = invalidSelection;
        const auto refused = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
            invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(refused.exit != 0, "malformed native environment selection was accepted");
    }
    for (const auto& invalidBody : {
        "public pure int Read(float value) => int.RoundFromFloat32(value);",
        "public pure int Read(float32 value) => int.RoundFromFloat32(value, 0);",
        "public pure int Read(string value) => string.ByteAtOr(value, 0);",
        "public pure bool Read(string value) => string.ByteLengthIs(value, 0.0);",
        "public pure int Read(float value) => float.ILogB(value, 1);",
        "public pure int Read(float32 value) => float.ILogB(value);",
        "public pure float Read(float value) => float.ScaleB(value, 1.0);",
        "public pure float32 Read(float32 value) => float32.ScaleB(value);"}) {
        auto invalid = selected;
        invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() +
            "\npublic model Class InvalidExponent { " + invalidBody + " }\n";
        const auto refused = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
            invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(refused.exit == 0 && !parseJson(refused.output, 4U << 20).at("ok").get<bool>(),
            "invalid exponent API signature was admitted");
    }
    auto legacy = Json{{"protocol", "pipelang.native-model.v1"}, {"source", "public Class Empty {}"},
        {"native_numeric_environment", "active-host"}};
    require(process({nativeExecutable(prefix / "bin/pipelang-modelc").string()}, legacy.dump(),
        std::chrono::seconds(45), 4U << 20).exit != 0, "nonproject environment selection was accepted");
    auto forged = model;
    forged.erase("nativeNumericEnvironment");
    bool refused = false;
    try {
        admitModel(forged, "active_header");
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "active export metadata was admitted without project selection");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto tool = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto hash = fileHash(manifest);
    const auto defaultPlan = compiledProjectInformation(prefix, manifest, hash, tool,
        output, "src", "Queries.pipe", "active_compiled");
    const auto activePlan = compiledProjectInformation(prefix, manifest, hash, tool,
        output, "src", "Queries.pipe", "active_compiled", "active-host");
    require(defaultPlan.at("plan_sha256") != activePlan.at("plan_sha256") &&
        activePlan.at("native_numeric_environment") == "active-host", "active plan identity was not selected");
    refused = false;
    try {
        generateCompiledProjectBindings(prefix, manifest, hash, tool, output, "src", "Queries.pipe",
            "active_compiled", output / "wrong-plan", defaultPlan.at("plan_sha256").get<std::string>(), "active-host");
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused && !fs::exists(output / "wrong-plan/Model.hpp"), "cross-profile plan mismatch was published");
    atomicWrite(output / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(ActiveNumericFixture LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(active_header PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" SOURCE_ROOT src ENTRY Queries.pipe
    NAMESPACE active_header OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/header"
    NATIVE_NUMERIC_ENVIRONMENT active-host)
pipelang_add_model(active_compiled PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" SOURCE_ROOT src ENTRY Queries.pipe
    NAMESPACE active_compiled OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/compiled"
    COMPILATION COMPILED NATIVE_NUMERIC_ENVIRONMENT active-host)
add_executable(active_numeric main.cpp)
target_link_libraries(active_numeric PRIVATE active_header active_compiled)
target_include_directories(active_numeric PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(active_numeric PRIVATE -Wall -Wextra -Werror)
option(ACTIVE_SANITIZERS "Instrument active numeric callers and ordinary implementations" OFF)
if(ACTIVE_SANITIZERS)
    foreach(target active_numeric active_compiled_implementation)
        target_compile_options(${target} PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(active_numeric PRIVATE -fsanitize=address,undefined)
endif()
)cmake");
    atomicWrite(output / "main.cpp", R"cpp(#include "header/Model.hpp"
#include "compiled/Model.hpp"
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

bool refuseAllocations = false;
void* operator new(std::size_t size) {
    if (refuseAllocations)
        std::abort();
    if (void* value = std::malloc(size))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

struct EnvironmentRestore {
    std::fenv_t environment;
#if defined(__SSE2__)
    unsigned control = _mm_getcsr();
#endif
    EnvironmentRestore() { std::fegetenv(&environment); }
    ~EnvironmentRestore() {
        std::fesetenv(&environment);
#if defined(__SSE2__)
        _mm_setcsr(control);
#endif
    }
};

template<class Operation, class Reference>
bool sameFloating(Operation operation, Reference reference) {
    std::fenv_t initial;
    std::fegetenv(&initial);
#if defined(__SSE2__)
    const unsigned initialControl = _mm_getcsr();
#endif
    // Complete both calculations before inspecting their floating status. Pure
    // native math calls may otherwise move past the observation at optimization.
    volatile auto expectedStorage = reference();
    const auto expected = expectedStorage;
    const int expectedFlags = std::fetestexcept(FE_ALL_EXCEPT);
    const int expectedRounding = std::fegetround();
#if defined(__i386__) || defined(__x86_64__)
    unsigned short expectedX87Control = 0;
    asm volatile("fnstcw %0" : "=m"(expectedX87Control));
#endif
#if defined(__SSE2__)
    const unsigned expectedControl = _mm_getcsr();
#endif
    std::fesetenv(&initial);
#if defined(__SSE2__)
    _mm_setcsr(initialControl);
#endif
    refuseAllocations = true;
    volatile auto actualStorage = operation();
    const auto actual = actualStorage;
    refuseAllocations = false;
    const int actualFlags = std::fetestexcept(FE_ALL_EXCEPT);
    bool sameControl = std::fegetround() == expectedRounding;
#if defined(__SSE2__)
    sameControl = sameControl && _mm_getcsr() == expectedControl;
#endif
#if defined(__i386__) || defined(__x86_64__)
    unsigned short actualX87Control = 0;
    asm volatile("fnstcw %0" : "=m"(actualX87Control));
    sameControl = sameControl && actualX87Control == expectedX87Control;
#endif
    using Bits = std::conditional_t<sizeof(actual) == 4, std::uint32_t, std::uint64_t>;
    const auto actualBits = std::bit_cast<Bits>(actual);
    const auto expectedBits = std::bit_cast<Bits>(expected);
    const bool matches = actualBits == expectedBits && actualFlags == expectedFlags && sameControl;
    if (!matches)
        std::fprintf(stderr, "floating mismatch: actual=%llx expected=%llx flags=%x/%x controls=%d rounding=%d\n",
            static_cast<unsigned long long>(actualBits), static_cast<unsigned long long>(expectedBits),
            actualFlags, expectedFlags, sameControl, std::fegetround());
    return matches;
}

#include "ExponentChecks.hpp"

template<class Query>
bool verifyTrigonometry() {
    const auto check = [](auto operation, auto reference) {
        // Each call starts with fresh observations so earlier libm flags cannot
        // conceal a missing exception in a later sine or cosine operation.
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        return sameFloating(operation, reference);
    };
    static_assert(noexcept(Query::Transcendental(0.f)));
    static_assert(noexcept(Query::SinSingle(0.f)));
    static_assert(noexcept(Query::CosSingle(0.f)));
    static_assert(noexcept(Query::SinWide(0.0)));
    static_assert(noexcept(Query::CosWide(0.0)));
    static_assert(noexcept(Query::LiteralSinSingle()));
    static_assert(noexcept(Query::LiteralCosSingle()));
    static_assert(noexcept(Query::LiteralSinWide()));
    static_assert(noexcept(Query::LiteralCosWide()));
    const std::array<std::uint32_t, 28> singleWords{0u, 0x80000000u, 1u, 0x80000001u,
        0x007fffffu, 0x00800000u, 0x3f000000u, 0xbf000000u, 0x3f800000u, 0xbf800000u,
        0x40490fdau, 0x40490fdbu, 0x40490fdcu, 0xc0490fdbu,
        0x40c90fdau, 0x40c90fdbu, 0x40c90fdcu, 0xc0c90fdbu,
        0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
        0x7fc12345u, 0xffc12345u, 0x7f812345u, 0xff812345u, 0x3fc90fdbu, 0xbfc90fdbu};
    for (const auto word : singleWords) {
        const float value = std::bit_cast<float>(word);
        const auto sine = [&] {
            volatile float input = value;
            volatile float result = std::sin(input);
            return result;
        };
        const auto cosine = [&] {
            volatile float input = value;
            volatile float result = std::cos(input);
            return result;
        };
        if (!check([&] { return Query::SinSingle(value); }, sine) ||
            !check([&] { return Query::Transcendental(value); }, sine) ||
            !check([&] { return Query::CosSingle(value); }, cosine)) {
            std::fprintf(stderr, "single trigonometry input=%08x\n", word);
            return false;
        }
    }
    const std::array<std::uint64_t, 28> wideWords{0u, UINT64_C(0x8000000000000000),
        1u, UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x3fe0000000000000), UINT64_C(0xbfe0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
        UINT64_C(0x400921fb54442d17), UINT64_C(0x400921fb54442d18), UINT64_C(0x400921fb54442d19), UINT64_C(0xc00921fb54442d18),
        UINT64_C(0x401921fb54442d17), UINT64_C(0x401921fb54442d18), UINT64_C(0x401921fb54442d19), UINT64_C(0xc01921fb54442d18),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff8123456789abc), UINT64_C(0x7ff0123456789abc), UINT64_C(0xfff0123456789abc),
        UINT64_C(0x3ff921fb54442d18), UINT64_C(0xbff921fb54442d18)};
    for (const auto word : wideWords) {
        const double value = std::bit_cast<double>(word);
        if (!check([&] { return Query::SinWide(value); }, [&] {
                volatile double input = value;
                volatile double result = std::sin(input);
                return result;
            }) || !check([&] { return Query::CosWide(value); }, [&] {
                volatile double input = value;
                volatile double result = std::cos(input);
                return result;
            })) {
            std::fprintf(stderr, "wide trigonometry input=%016llx\n", static_cast<unsigned long long>(word));
            return false;
        }
    }
    return check([] { return Query::LiteralSinSingle(); }, [] {
            volatile float input = 1.f;
            volatile float result = std::sin(input);
            return result;
        }) && check([] { return Query::LiteralCosSingle(); }, [] {
            volatile float input = 1.f;
            volatile float result = std::cos(input);
            return result;
        }) && check([] { return Query::LiteralSinWide(); }, [] {
            volatile double input = 1.0;
            volatile double result = std::sin(input);
            return result;
        }) && check([] { return Query::LiteralCosWide(); }, [] {
            volatile double input = 1.0;
            volatile double result = std::cos(input);
            return result;
        });
}

template<class Query>
bool verifyActive() {
    static_assert(noexcept(Query::SingleFromInteger(0)));
    static_assert(noexcept(Query::SquareRootSingle(0.f)));
    static_assert(noexcept(Query::SquareRootWide(0.0)));
    const std::array<std::uint32_t, 17> squareRootWords{0u, 0x80000000u, 1u, 0x80000001u,
        0x007fffffu, 0x00800000u, 0x3f000000u, 0x3f800000u, 0x40000000u,
        0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
        0x7fc12345u, 0xffc12345u, 0x7f812345u};
    for (const auto word : squareRootWords) {
        const float value = std::bit_cast<float>(word);
        if (!sameFloating([&] { return Query::SquareRootSingle(value); }, [&] {
                volatile float input = value;
                volatile float result = std::sqrt(input);
                return result;
            }))
            return false;
    }
    const std::array<std::uint64_t, 17> squareRootWideWords{0u, UINT64_C(0x8000000000000000), 1u,
        UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x3fe0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
        UINT64_C(0xbff0000000000000), UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8123456789abc),
        UINT64_C(0xfff8123456789abc), UINT64_C(0x7ff0123456789abc)};
    for (const auto word : squareRootWideWords) {
        const double value = std::bit_cast<double>(word);
        if (!sameFloating([&] { return Query::SquareRootWide(value); }, [&] {
                volatile double input = value;
                volatile double result = std::sqrt(input);
                return result;
            }))
            return false;
    }
    if (!sameFloating([] { return Query::LiteralSquareRootSingle(); }, [] {
            volatile float input = 2.f;
            volatile float result = std::sqrt(input);
            return result;
        }) || !sameFloating([] { return Query::LiteralSquareRootWide(); }, [] {
            volatile double input = 2.0;
            volatile double result = std::sqrt(input);
            return result;
        }))
        return false;
    static_assert(noexcept(Query::Truncate(0.0f)));
    static_assert(noexcept(Query::RoundSingle(0.0f)));
    static_assert(noexcept(Query::RoundWide(0.0)));
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    if (!sameFloating([] { return Query::LiteralRoundSingle(); }, [] {
            volatile float value = 0x1p-149f;
            return static_cast<std::int64_t>(std::lround(value));
        }))
        return false;
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    if (!sameFloating([] { return Query::LiteralRoundWide(); }, [] {
            volatile double value = 0x1p-1074;
            return static_cast<std::int64_t>(std::lround(value));
        }))
        return false;
    for (const double value : {-65535.0, -1.5, -0.5, -0.0, 0.0, 0.5, 1.5, 65535.0,
                              0x1p-149, -0x1p-149, 0x1p-1074, -0x1p-1074}) {
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        if (!sameFloating([&] { return Query::RoundWide(value); }, [&] {
                volatile double input = value;
                if (!std::isfinite(input) || input < -65535.0 || input > 65535.0)
                    return INT64_C(-1);
                return static_cast<std::int64_t>(std::lround(input));
            }))
            return false;
        const float single = static_cast<float>(value);
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        if (!sameFloating([&] { return Query::RoundSingle(single); }, [&] {
                volatile float input = single;
                if (!std::isfinite(input) || input < -65535.0f || input > 65535.0f)
                    return INT64_C(-1);
                return static_cast<std::int64_t>(std::lround(input));
            }))
            return false;
    }
    for (const float value : {-1.0f, -0.5f, -0x1p-149f, -0.0f, 0.0f, 0x1p-149f, 0.5f, 1.0f}) {
        for (const float scale : {-65535.0f, -1.5f, -0x1p-149f, 0.0f, 0x1p-149f, 1.5f, 65535.0f}) {
            std::feclearexcept(FE_ALL_EXCEPT);
            std::feraiseexcept(FE_DIVBYZERO);
            if (!sameFloating([&] { return Query::RoundProduct(value, scale); }, [&] {
                    volatile float input = value;
                    volatile float multiplier = scale;
                    if (!std::isfinite(input) || input < -1.0f || input > 1.0f ||
                        !std::isfinite(multiplier) || multiplier < -65535.0f || multiplier > 65535.0f)
                        return INT64_C(-1);
                    volatile float product = input * multiplier;
                    return static_cast<std::int64_t>(std::lround(product));
                }))
                return false;
        }
    }
    for (const std::int64_t value : {INT64_MIN, INT64_MAX, INT64_C(-16777217), INT64_C(16777217),
                                   INT64_C(-65535), INT64_C(0), INT64_C(65535)}) {
        if (!sameFloating([&] { return Query::SingleFromInteger(value); }, [&] {
                volatile std::int64_t input = value;
                volatile float result = static_cast<float>(input);
                return result;
            }) || !sameFloating([&] { return Query::WideFromInteger(value); }, [&] {
                volatile std::int64_t input = value;
                volatile double result = static_cast<double>(input);
                return result;
            }))
            return false;
    }
    for (const std::int64_t value : {INT64_C(1), INT64_C(32767), INT64_C(65534), INT64_C(65535)}) {
        if (!sameFloating([&] { return Query::Normalize(value); }, [&] {
                volatile std::int64_t input = value;
                volatile float numerator = static_cast<float>(input);
                volatile float denominator = 65535.0f;
                volatile float result = numerator / denominator;
                return result;
            }))
            return false;
    }
    for (const float value : {0x1p-149f, 0x1p-126f, -0.0f, 0.0f, 1.0f, 65535.0f,
                             std::bit_cast<float>(0x7fc12345U), std::bit_cast<float>(0xffc12345U),
                             std::bit_cast<float>(0x7f812345U), std::bit_cast<float>(0xff812345U)}) {
        if (!sameFloating([&] { return Query::Divide(value, 3.0f); }, [&] {
                volatile float first = value;
                volatile float second = 3.0f;
                volatile float result = first / second;
                return result;
            }) || !sameFloating([&] { return Query::Multiply(value, 1.0f); }, [&] {
                volatile float first = value;
                volatile float second = 1.0f;
                volatile float result = first * second;
                return result;
            }) || !sameFloating([&] { return Query::Add(value, 0x1p-24f); }, [&] {
                volatile float first = value;
                volatile float second = 0x1p-24f;
                volatile float result = first + second;
                return result;
            }))
            return false;
    }
    for (const auto bits : {1U, 0x80000001U, 0x7fc12345U, 0xffc12345U, 0x7f812345U, 0xff812345U}) {
        const float value = std::bit_cast<float>(bits);
        if (!sameFloating([&] { return Query::Widen(value); }, [&] {
                volatile float input = value;
                volatile double converted = static_cast<double>(input);
                const auto convertedBits = std::bit_cast<std::uint64_t>(static_cast<double>(converted));
                const bool nan = (convertedBits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)
                    && (convertedBits & UINT64_C(0x000fffffffffffff)) != 0;
                return nan ? std::bit_cast<double>((convertedBits & UINT64_C(0x8000000000000000)) |
                    UINT64_C(0x7ff8000000000000)) : static_cast<double>(converted);
            }))
            return false;
    }
    if (!sameFloating([] { return Query::LiteralFloor(); }, [] {
            volatile double value = 1.5;
            return std::floor(value);
        }))
        return false;
    if (!sameFloating([] { return Query::LiteralDivide(); }, [] {
            volatile float first = 1.0f;
            volatile float second = 3.0f;
            volatile float result = first / second;
            return result;
        }) || !sameFloating([] { return Query::WideAdd(1.0, 0x1p-53); }, [] {
            volatile double first = 1.0;
            volatile double second = 0x1p-53;
            volatile double result = first + second;
            return result;
        }))
        return false;
    refuseAllocations = true;
    for (const float value : {-65535.0f, -1.75f, -0.0f, 0.0f, 1.75f, 65535.0f}) {
        const auto converted = Query::Truncate(value);
        if (!converted.is_success() || converted.value() != static_cast<std::int64_t>(value))
            std::abort();
    }
    for (const float value : {0.0f, 0.5f, 1.5f, 32767.5f, 65535.0f}) {
        const auto rounded = Query::Nearest(value);
        if (!rounded.is_success() || rounded.value() !=
                static_cast<std::int64_t>(std::floor(static_cast<double>(value) + 0.5)))
            std::abort();
    }
    if (!Query::Truncate(std::numeric_limits<float>::infinity()).is_failure() ||
        !Query::Truncate(std::numeric_limits<float>::quiet_NaN()).is_failure() ||
        !Query::Truncate(65536.0f).is_failure())
        std::abort();
    refuseAllocations = false;
    return true;
}

template<class Query>
bool verifyConstantsAndQualifiedHelpers() {
    // These independent constant expressions are evaluated at translation time.
    // Runtime divisions above intentionally continue observing the active mode.
    constexpr float oneThird = 1.0f / 3.0f;
    constexpr float twoThirds = 2.0f / 3.0f;
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    const int initialRounding = std::fegetround();
    const int initialFlags = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__SSE2__)
    const unsigned initialControl = _mm_getcsr();
#endif
    if (!sameFloating([] { return Query::ConstantOneThird(); }, [] { return oneThird; }) ||
        !sameFloating([] { return Query::ConstantTwoThirds(); }, [] { return twoThirds; }))
        return false;
    if (std::fetestexcept(FE_ALL_EXCEPT) != initialFlags || std::fegetround() != initialRounding)
        return false;
#if defined(__SSE2__)
    if (_mm_getcsr() != initialControl)
        return false;
#endif
    for (const float value : {0x1p-149f, -0.0f, 0.0f, 0.5f, 1.0f,
                             std::bit_cast<float>(0x7fc12345U), std::bit_cast<float>(0x7f812345U)}) {
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        if (!sameFloating([&] { return Query::QualifiedAddThird(value); }, [&] {
                volatile float first = value;
                volatile float second = oneThird;
                volatile float result = first + second;
                return result;
            }))
            return false;
    }
    for (const std::int64_t value : {INT64_C(1), INT64_C(32767), INT64_C(65534), INT64_C(65535)}) {
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        if (!sameFloating([&] { return Query::QualifiedNormalize(value); }, [&] {
                volatile std::int64_t input = value;
                volatile float numerator = static_cast<float>(input);
                volatile float denominator = 65535.0f;
                volatile float result = numerator / denominator;
                return result;
            }))
            return false;
    }
    for (const float value : {0.0f, 0.5f, 1.5f, 32767.5f, 65535.0f}) {
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        if (!sameFloating([&] {
                const auto result = Query::QualifiedNearest(value);
                if (!result.is_success())
                    std::abort();
                return result.value();
            }, [&] {
                volatile float input = value;
                volatile double widened = static_cast<double>(input);
                volatile double half = 0.5;
                volatile double shifted = widened + half;
                volatile double rounded = std::floor(shifted);
                volatile std::int64_t result = static_cast<std::int64_t>(rounded);
                return result;
            }))
            return false;
    }
    refuseAllocations = true;
    const auto failed = Query::QualifiedNearest(std::numeric_limits<float>::infinity());
    refuseAllocations = false;
    using Error = std::remove_cvref_t<decltype(failed.error())>;
    return failed.is_failure() && failed.error() == Error::Invalid && std::fegetround() == initialRounding;
}

template<class Query, class Channels, class Error>
bool verifyOutcomes() {
    using Result = pipelang::model::Result<Channels, Error>;
    const auto cancelled = Result::Cancelled(Error::Interrupted);
    const auto failed = Result::Failure(Error::Invalid);
    const auto success = Result::Success(Channels{1, 2, 3});
    refuseAllocations = true;
    const auto returnedCancellation = Query::Forward(cancelled);
    const auto returnedFailure = Query::Forward(failed);
    const auto returnedSuccess = Query::Forward(success);
    refuseAllocations = false;
    return returnedCancellation.is_cancelled() && returnedCancellation.error() == Error::Interrupted &&
        returnedFailure.is_failure() && returnedFailure.error() == Error::Invalid &&
        returnedSuccess.is_success() && returnedSuccess.value().Third == 3;
}

int main() {
    EnvironmentRestore restore;
    active_header::source::Active::Queries headerOrdinary;
    active_compiled::source::Active::Queries compiledOrdinary;
    using Header = active_header::native_numeric::Active::Queries;
    using Compiled = active_compiled::native_numeric::Active::Queries;
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        if (std::fesetround(rounding) != 0)
            return 2;
#if defined(__SSE2__)
        const unsigned initial = _mm_getcsr();
        for (const unsigned roundingBits : {0U, 0x2000U, 0x4000U, 0x6000U}) {
            for (const unsigned denormals : {0U, 0x40U, 0x8000U, 0x8040U}) {
                _mm_setcsr((initial & ~0xe040U) | roundingBits | denormals);
#endif
                std::feclearexcept(FE_ALL_EXCEPT);
                std::feraiseexcept(FE_DIVBYZERO);
                if (!verifyExponentOperations<Header>() || !verifyExponentOperations<Compiled>()) {
                    std::fprintf(stderr, "exponent/narrowing values/status mismatch\n");
                    return 7;
                }
                if (!verifyTrigonometry<Header>() || !verifyTrigonometry<Compiled>()) {
                    std::fprintf(stderr, "active trigonometry values/status mismatch\n");
                    return 8;
                }
                if (!verifyActive<Header>() || !verifyActive<Compiled>()) {
                    std::fprintf(stderr, "active floating values/status mismatch\n");
                    return 3;
                }
                if (!verifyConstantsAndQualifiedHelpers<Header>() || !verifyConstantsAndQualifiedHelpers<Compiled>()) {
                    std::fprintf(stderr, "active constants/qualified helpers values/status mismatch\n");
                    return 6;
                }
                bool ordinarySupported = rounding == FE_TONEAREST;
#if defined(__SSE2__)
                ordinarySupported = ordinarySupported && roundingBits == 0 && denormals == 0;
#endif
                const auto checkOrdinary = [ordinarySupported](auto& model) {
                    bool rejected = false;
                    try {
                        if (model.SingleFromInteger(1) != 1.0f)
                            return false;
                        if (ordinarySupported && (model.WideExponent(0.5) != -1 ||
                            model.SingleExponent(0x1p-126f) != -126 || model.WideScale(0.5, 3) != 4.0 ||
                            model.SingleScale(0.5f, 3) != 4.0f ||
                            std::bit_cast<std::uint32_t>(model.Narrow(0x1.ffffffp127)) != 0x7f800000U))
                            return false;
                    } catch (const pipelang::model::ExecutionFailure&) {
                        rejected = true;
                    }
                    return rejected != ordinarySupported;
                };
                if (!checkOrdinary(headerOrdinary) || !checkOrdinary(compiledOrdinary))
                    return 4;
#if defined(__SSE2__)
            }
        }
#endif
    }
    if (!verifyOutcomes<Header, active_header::source::Active::Channels, active_header::source::Active::NumericError>() ||
        !verifyOutcomes<Compiled, active_compiled::source::Active::Channels, active_compiled::source::Active::NumericError>())
        return 5;
    return 0;
}
)cpp");
}
