// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeStructuredValueFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include <set>

namespace {
using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command)
{
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "structured value fixture failed (" + std::to_string(result.exit) + "): " + result.output + result.diagnostic);
}
}

void nativeStructuredValueContracts(const std::filesystem::path& prefix,
                                    const std::filesystem::path& output)
{
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Queries.pipe", R"pipe(module ClosedValues;
public Enum Code { None, Rejected, Interrupted }
public Enum Cause { None, First, Second }
public Record ZLeaf {
    public int Number;
    public uint32 Bits;
}
public Record AContext {
    public ZLeaf Leaf;
    public Cause? Origin;
}
public Record BError {
    public Code Code;
    public AContext Context;
}
public Record Payload {
    public float32 Marker;
    public AContext Context;
}
public Record DefaultedError {
    public AContext Context = new AContext { Origin = Cause.First };
}
public Record DynamicError {
    public List<int> Values;
}
public Record UnsafeDefaultError {
    public int Number = List<int>.Empty().At(0);
}
public model Class Queries {
    private pure BError CreateError(Code code, Cause? origin, int number, uint32 bits)
        => new BError { Code = code, Context = new AContext {
            Leaf = new ZLeaf { Number = number, Bits = bits }, Origin = origin } };

    public pure Result<Payload, BError> Make(float32 marker, uint32 bits, int state, bool hasCause) {
        Cause? origin = hasCause ? Cause.Second : null;
        if (state < 0)
            return Result<Payload, BError>.Cancelled(CreateError(Code.Interrupted, origin, state, bits));
        if (state == 0)
            return Result<Payload, BError>.Failure(CreateError(Code.Rejected, origin, state, bits));
        return Result<Payload, BError>.Success(new Payload {
            Marker = marker,
            Context = new AContext { Leaf = new ZLeaf { Number = state, Bits = bits }, Origin = origin }
        });
    }

    public pure Result<int, DefaultedError> DefaultSuccess() => Result<int, DefaultedError>.Success(7);
    public pure Result<void, DefaultedError> DefaultCancelled() => Result<void, DefaultedError>.Cancelled();
    public pure Result<Payload, BError> Forward(Result<Payload, BError> value) => value;
    public pure Result<uint32, BError> Convert(Result<Payload, BError> value) {
        if (value.IsCancelled)
            return Result<uint32, BError>.Cancelled(value.Error);
        if (value.IsFailure)
            return Result<uint32, BError>.Failure(value.Error);
        return Result<uint32, BError>.Success(value.Value.Context.Leaf.Bits);
    }
    public pure Result<void, BError> Complete(int state, bool hasCause) {
        Cause? origin = hasCause ? Cause.First : null;
        if (state < 0)
            return Result<void, BError>.Cancelled(CreateError(Code.Interrupted, origin, state, 0u));
        if (state == 0)
            return Result<void, BError>.Failure(CreateError(Code.Rejected, origin, state, 0u));
        return Result<void, BError>.Success();
    }
    public pure Result<int, BError> Checked(int value) {
        if (value < 0)
            return Result<int, BError>.Failure(CreateError(Code.Rejected, Cause.First, value, 7u));
        return Result<int, BError>.Success(value);
    }
    public pure Cause ErrorCause(Result<Payload, BError> value) {
        if (value.Error.Context.Origin.HasValue)
            return value.Error.Context.Origin.Value;
        return Cause.None;
    }
    public pure Cause PayloadCause(Result<Payload, BError> value) {
        if (!value.IsSuccess)
            return Cause.None;
        if (!value.Value.Context.Origin.HasValue)
            return Cause.None;
        return value.Value.Context.Origin.Value;
    }
    public pure Cause Coalesce(Cause? value, Cause fallback)
        => value.HasValue ? value.Value : fallback;
    public pure bool Both(Cause? value) => value.HasValue && value.Value == Cause.First;
    public pure bool AbsentOrFirst(Cause? value) => !value.HasValue || value.Value == Cause.First;
    public pure Cause Nested(AContext? value) {
        if (value.HasValue && value.Value.Origin.HasValue)
            return value.Value.Origin.Value;
        return Cause.None;
    }
    public pure int? Lift(int value, bool enabled) => enabled ? value : null;
    public pure int? ArrayLift(int value) {
        Array<int> values = Array<int>.Create(1, value);
        return values.At(0);
    }
    public pure int ReadNumber(int? value) {
        if (!value.HasValue)
            return 0;
        return value.Value;
    }
    public pure int? CopyOptional(int? value) => value;
    public pure BError CopyError(BError value) => value;
    public pure Payload CopyPayload(Payload value) => value;

    public pure Cause Unsafe(Cause? value) => value.Value;
    public pure Cause Joined(Cause? value, bool enabled) {
        if (enabled && value.HasValue) {
        }
        return value.Value;
    }
    private pure Cause? Fresh(Cause value) => value;
    public pure Cause Repeated(Cause value) {
        if (Fresh(value).HasValue)
            return Fresh(value).Value;
        return Cause.None;
    }
    public pure Result<int, DynamicError> Dynamic() => Result<int, DynamicError>.Failure(new DynamicError { });
    public pure Result<int, UnsafeDefaultError> HiddenDefault() => Result<int, UnsafeDefaultError>.Success(1);
    public pure Result<void, UnsafeDefaultError> HiddenCancelled() => Result<void, UnsafeDefaultError>.Cancelled();
}
)pipe");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto project = SourceProject::load(output, "src", "Queries.pipe");
    auto request = project.request("structured_header");
    const auto response = process({compiler.string()}, request.dump(), std::chrono::seconds(45), 4U << 20);
    require(response.exit == 0, "structured compiler transport failed");
    const auto parsed = parseJson(response.output, 4U << 20);
    require(parsed.at("ok") == true, "structured source refused: " + parsed.dump());
    const auto& model = parsed.at("manifest");
    std::set<std::string> methods;
    for (const auto& item : model.at("nativeNumericExports")) {
        if (item.at("owner") == "ClosedValues.Queries")
            methods.insert(item.at("method").get<std::string>());
    }
    const std::set<std::string> expected{"Make", "Forward", "Convert", "Complete", "Checked", "ErrorCause",
        "PayloadCause", "Coalesce", "Both", "AbsentOrFirst", "Nested", "Lift", "ArrayLift", "ReadNumber",
        "CopyOptional", "CopyError", "CopyPayload", "DefaultSuccess", "DefaultCancelled"};
    require(methods == expected, "structured projection admitted an unsafe read/default or omitted a proved closed value: " + canonical(methods));
    admitModel(model, "structured_header");
    for (const auto& replacement : {"List<int>", "UnknownError", "ClosedValues.BError"}) {
        auto invalid = model;
        for (auto& record : invalid["records"]) {
            if (record.value("sourceName", "") == "ClosedValues.AContext")
                record["fields"][0]["type"] = replacement;
        }
        bool refused = false;
        try {
            admitModel(invalid, "structured_header");
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "structured numeric manifest admitted a dynamic, undeclared or cyclic record channel");
    }
    for (const auto& alias : {"List<int>", "string", "ClosedValues.AContext", "Lock",
                             "ConcurrentDictionary", "ConcurrentQueue", "ConcurrentStack"}) {
        auto invalid = model;
        invalid["enums"].push_back(Json{{"name", "DynamicAlias"}, {"sourceName", alias}, {"values", Json::array({"Value"})}});
        for (auto& record : invalid["records"]) {
            if (record.value("sourceName", "") == "ClosedValues.AContext")
                record["fields"][0]["type"] = alias;
        }
        bool refused = false;
        try {
            admitModel(invalid, "structured_header");
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "closed numeric manifest trusted a reserved, generic or ambiguous authored alias");
    }
    for (const auto& invalidMethod : {
        "public async Task<Result<int,BError>> Invalid() { return Result<int,BError>.Success(1); }",
        "public pure Result<int,int> Invalid() => Result<int,int>.Failure(1);"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() +
            "\npublic model Class InvalidBoundary { " + invalidMethod + " }\n";
        const auto rejected = process({compiler.string()}, invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(rejected.exit == 0 && parseJson(rejected.output, 4U << 20).at("ok") == false,
            "structured errors widened an async or scalar-error boundary: " + std::string(invalidMethod));
    }
    auto ordinaryResult = request;
    ordinaryResult["modules"][0]["source"] = ordinaryResult["modules"][0]["source"].get<std::string>() +
        "\npublic model Class InvalidBoundary { "
        "public Result<int,BError> Invalid() => Result<int,BError>.Success(1); }\n";
    const auto ordinaryCompiled = process({compiler.string()}, ordinaryResult.dump(), std::chrono::seconds(45), 4U << 20);
    require(ordinaryCompiled.exit == 0, "ordinary structured Result compiler transport failed");
    const auto ordinaryResponse = parseJson(ordinaryCompiled.output, 4U << 20);
    require(ordinaryResponse.at("ok") == true, "valid ordinary structured Result method was rejected");
    for (const auto& item : ordinaryResponse.at("manifest").at("nativeNumericExports")) {
        require(item.at("owner") != "ClosedValues.InvalidBoundary",
            "non-pure structured Result method escaped through a pure native numeric export");
    }
    generateProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "structured_header", output / "header");
    const auto compiledInfo = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "structured_compiled");
    const auto compiledReceipt = generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "structured_compiled", output / "compiled", compiledInfo.at("plan_sha256").get<std::string>());
    (void)compiledReceipt;
    generateProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "structured_active", output / "active", "active-host");
    const auto activeInfo = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "structured_active_compiled", "active-host");
    generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "structured_active_compiled", output / "active-compiled",
        activeInfo.at("plan_sha256").get<std::string>(), "active-host");
    const auto lean = readFile(output / "header/Numeric.hpp", 4U << 20);
    require(lean.find("runtime.hpp") == std::string::npos && lean.find("object.hpp") == std::string::npos,
        "closed structured header imported a model runtime");
    atomicWrite(output / "checks.hpp", R"cpp(#pragma once
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
inline bool forbidStructuredAllocations = false;
inline std::size_t structuredChecks = 0;
#if defined(__SSE2__)
inline constexpr std::size_t structuredProfileCount = 64;
#else
inline constexpr std::size_t structuredProfileCount = 4;
#endif

template<class Query>
concept ExposesUnsafeOptional = requires { &Query::Unsafe; };
template<class Query>
concept ExposesJoinedOptional = requires { &Query::Joined; };
template<class Query>
concept ExposesRepeatedOptional = requires { &Query::Repeated; };
template<class Query>
concept ExposesUnsafeErrorDefault = requires { &Query::HiddenDefault; };

template<class Query>
bool verifyStructuredValues()
{
    static_assert(!ExposesUnsafeOptional<Query> && !ExposesJoinedOptional<Query>);
    static_assert(!ExposesRepeatedOptional<Query> && !ExposesUnsafeErrorDefault<Query>);
    static_assert(noexcept(Query::Make(0.f, 0u, 0, false)));
    static_assert(noexcept(Query::Complete(0, false)));
    using Outcome = decltype(Query::Make(0.f, 0u, 0, false));
    using Error = std::remove_cvref_t<decltype(std::declval<Outcome>().error())>;
    using Cause = std::remove_cvref_t<decltype(std::declval<Error>().Context.Origin.value())>;
    using Code = decltype(Error{}.Code);
    const auto defaultSuccess = Query::DefaultSuccess();
    const auto defaultCancelled = Query::DefaultCancelled();
    if (!defaultSuccess.is_success() || defaultSuccess.value() != 7 || !defaultCancelled.is_cancelled()
        || !defaultSuccess.error().Context.Origin.has_value()
        || defaultSuccess.error().Context.Origin.value() != Cause::First
        || !defaultCancelled.error().Context.Origin.has_value()
        || defaultCancelled.error().Context.Origin.value() != Cause::First)
        return false;
    const std::array<std::uint32_t, 12> words{0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu,
        0x00800000u, 0x3f000000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f812345u};
    for (const auto bits : words) {
        const float marker = std::bit_cast<float>(bits);
        for (const std::int64_t state : {-7, 0, 3}) {
            for (const bool hasCause : {false, true}) {
                const auto result = Query::Make(marker, bits, state, hasCause);
                const auto forwarded = Query::Forward(result);
                const auto converted = Query::Convert(result);
                const auto completed = Query::Complete(state, hasCause);
                if (result.outcome() != forwarded.outcome() || result.outcome() != converted.outcome()
                    || result.outcome() != completed.outcome())
                    return false;
                if (state > 0) {
                    if (!result.is_success() || std::bit_cast<std::uint32_t>(result.value().Marker) != bits
                        || result.value().Context.Leaf.Number != state || result.value().Context.Leaf.Bits != bits
                        || result.value().Context.Origin.has_value() != hasCause || converted.value() != bits)
                        return false;
                    const auto copied = Query::CopyPayload(result.value());
                    if (std::bit_cast<std::uint32_t>(copied.Marker) != bits || copied.Context.Leaf.Bits != bits)
                        return false;
                    if (result.error().Code != Code::None || result.error().Context.Origin.has_value()
                        || Query::ErrorCause(result) != Cause::None)
                        return false;
                    if (Query::PayloadCause(result) != (hasCause ? Cause::Second : Cause::None))
                        return false;
                } else {
                    if (result.is_cancelled() != (state < 0) || result.is_failure() != (state == 0))
                        return false;
                    const auto& error = result.error();
                    if (error.Code != (state < 0 ? Code::Interrupted : Code::Rejected)
                        || error.Context.Leaf.Number != state || error.Context.Leaf.Bits != bits
                        || error.Context.Origin.has_value() != hasCause || converted.error() != error
                        || forwarded.error() != error || Query::CopyError(error) != error)
                        return false;
                    if (hasCause && error.Context.Origin.value() != Cause::Second)
                        return false;
                    if (Query::ErrorCause(result) != (hasCause ? Cause::Second : Cause::None)
                        || Query::PayloadCause(result) != Cause::None)
                        return false;
                    if (completed.error().Context.Origin.has_value() != hasCause
                        || (hasCause && completed.error().Context.Origin.value() != Cause::First))
                        return false;
                }
                const auto optional = Query::Lift(state, hasCause);
                if (optional.has_value() != hasCause || Query::ReadNumber(optional) != (hasCause ? state : 0)
                    || Query::CopyOptional(optional).has_value() != hasCause)
                    return false;
                if (hasCause && Query::CopyOptional(optional).value() != state)
                    return false;
                if (!Query::ArrayLift(state).has_value() || Query::ArrayLift(state).value() != state)
                    return false;
                pipelang::model::Optional<Cause> cause;
                if (hasCause)
                    cause = pipelang::model::Optional<Cause>::Some(Cause::First);
                if (Query::Coalesce(cause, Cause::Second) != (hasCause ? Cause::First : Cause::Second)
                    || Query::Both(cause) != hasCause || !Query::AbsentOrFirst(cause))
                    return false;
                using Context = decltype(Error{}.Context);
                pipelang::model::Optional<Context> nested;
                if (hasCause)
                    nested = pipelang::model::Optional<Context>::Some(Context{{state, bits}, cause});
                if (Query::Nested(nested) != (hasCause ? Cause::First : Cause::None))
                    return false;
                ++structuredChecks;
            }
        }
    }
    return true;
}

template<class Operation>
bool structuredFloatingProfiles(Operation operation)
{
    fenv_t initial;
    if (std::fegetenv(&initial) != 0)
        return false;
#if defined(__SSE2__)
    const unsigned originalSse = _mm_getcsr();
#endif
    bool valid = true;
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        if (std::fesetround(rounding) != 0)
            return false;
#if defined(__SSE2__)
        for (unsigned sseRounding = 0; sseRounding < 4; ++sseRounding) {
            for (unsigned denormal = 0; denormal < 4; ++denormal) {
                const unsigned control = (_mm_getcsr() & ~0xe07fu) | (sseRounding << 13)
                    | ((denormal & 1u) ? 0x8000u : 0u) | ((denormal & 2u) ? 0x40u : 0u) | 5u;
                _mm_setcsr(control);
                const auto beforeRounding = std::fegetround();
                const auto beforeFlags = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__i386__) || defined(__x86_64__)
                unsigned short beforeX87 = 0;
                asm volatile("fnstcw %0" : "=m"(beforeX87));
#endif
                forbidStructuredAllocations = true;
                valid = operation() && valid;
                forbidStructuredAllocations = false;
                valid = valid && _mm_getcsr() == control && std::fegetround() == beforeRounding
                    && std::fetestexcept(FE_ALL_EXCEPT) == beforeFlags;
#if defined(__i386__) || defined(__x86_64__)
                unsigned short afterX87 = 0;
                asm volatile("fnstcw %0" : "=m"(afterX87));
                valid = valid && beforeX87 == afterX87;
#endif
            }
        }
#else
        forbidStructuredAllocations = true;
        valid = operation() && valid;
        forbidStructuredAllocations = false;
#endif
    }
    if (std::fesetenv(&initial) != 0)
        return false;
#if defined(__SSE2__)
    _mm_setcsr(originalSse);
#endif
    return valid;
}
)cpp");
    const std::string allocationTrap = R"cpp(
void* operator new(std::size_t bytes)
{
    if (forbidStructuredAllocations)
        std::abort();
    if (void* value = std::malloc(bytes))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
)cpp";
    atomicWrite(output / "main.cpp", R"cpp(#include "header/Model.hpp"
#include "compiled/Model.hpp"
#include "active/Model.hpp"
#include "active-compiled/Model.hpp"
#include "checks.hpp"
)cpp" + allocationTrap + R"cpp(
int main()
{
    using Query = structured_header::native_numeric::ClosedValues::Queries;
    using Compiled = structured_compiled::native_numeric::ClosedValues::Queries;
    using Active = structured_active::native_numeric::ClosedValues::Queries;
    using ActiveCompiled = structured_active_compiled::native_numeric::ClosedValues::Queries;
    // Ordinary execution retains throwing unsafe access/default behavior.
    structured_header::source::ClosedValues::Queries ordinary;
    const auto failure = ordinary.Make(-0.f, 23u, 0, true);
    if (!failure.is_failure() || failure.error().Context.Leaf.Bits != 23u
        || !failure.error().Context.Origin.has_value())
        return 1;
    using Cause = structured_header::source::ClosedValues::Cause;
    bool refusedOptional = false;
    try {
        (void)ordinary.Unsafe(pipelang::model::Optional<Cause>::None());
    } catch (const pipelang::model::ExecutionFailure&) {
        refusedOptional = true;
    }
    bool refusedDefault = false;
    try {
        (void)ordinary.HiddenDefault();
    } catch (const pipelang::model::ExecutionFailure&) {
        refusedDefault = true;
    }
    if (!refusedOptional || !refusedDefault)
        return 2;
    const bool valid = structuredFloatingProfiles([] {
        return verifyStructuredValues<Query>() && verifyStructuredValues<Compiled>()
            && verifyStructuredValues<Active>() && verifyStructuredValues<ActiveCompiled>();
    });
    return valid && structuredChecks == 288 * structuredProfileCount ? 0 : 3;
}
)cpp");
    atomicWrite(output / "standalone.cpp", R"cpp(#include "header/Numeric.hpp"
#include "compiled/Numeric.hpp"
#include "active/Numeric.hpp"
#include "active-compiled/Numeric.hpp"
#if defined(PIPELANG_ACTIVE_HOST_FLOATING)
#error Lean structured headers must not impose an active-host macro
#endif
#include "checks.hpp"
)cpp" + allocationTrap + R"cpp(
using Header = structured_header::numeric_header::native_numeric::ClosedValues::Queries;
using Compiled = structured_compiled::numeric_header::native_numeric::ClosedValues::Queries;
using Active = structured_active::numeric_header::native_numeric::ClosedValues::Queries;
using ActiveCompiled = structured_active_compiled::numeric_header::native_numeric::ClosedValues::Queries;
static_assert(Header::DefaultSuccess().error().Context.Origin.has_value());
static_assert(Compiled::DefaultCancelled().is_cancelled());
static_assert(Header::Checked(7).is_success() && Header::Checked(7).value() == 7);
static_assert(Header::Checked(-7).error().Context.Origin.has_value());
static_assert(Header::Complete(-1, true).is_cancelled());
static_assert(Compiled::Complete(1, false).is_success());
static_assert(Active::Checked(-1).is_failure());
static_assert(ActiveCompiled::Complete(0, true).error().Context.Origin.has_value());
int main()
{
    const bool valid = structuredFloatingProfiles([] {
        return verifyStructuredValues<Header>() && verifyStructuredValues<Compiled>()
            && verifyStructuredValues<Active>() && verifyStructuredValues<ActiveCompiled>();
    });
    return valid && structuredChecks == 288 * structuredProfileCount ? 0 : 1;
}
)cpp");
    atomicWrite(output / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.20)
project(StructuredValues LANGUAGES CXX)
find_package(Threads REQUIRED)
option(STRUCTURED_SANITIZERS "Instrument structured value consumers" OFF)
if(STRUCTURED_SANITIZERS)
    add_compile_options(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
add_executable(structured_values main.cpp)
add_executable(structured_headers standalone.cpp)
foreach(target structured_values structured_headers)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${STRUCTURED_SDK_PREFIX}/include")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endforeach()
target_link_libraries(structured_values PRIVATE Threads::Threads)
target_compile_definitions(structured_values PRIVATE PIPELANG_ACTIVE_HOST_FLOATING=1)
target_compile_options(structured_values PRIVATE -frounding-math)
)cmake");
    invoke({nativeTool("cmake").string(), "-S", output.string(), "-B", (output / "initial-normal").string(),
        "-DSTRUCTURED_SDK_PREFIX=" + prefix.string(),
        "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
        "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
        "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", (output / "initial-normal").string(), "--parallel", "1"});
    invoke({nativeExecutable(output / "initial-normal/structured_values").string()});
    invoke({nativeExecutable(output / "initial-normal/structured_headers").string()});
#if defined(__SSE2__)
    constexpr std::size_t profileCount = 64;
#else
    constexpr std::size_t profileCount = 4;
#endif
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"normal", true}, {"structured_errors", true}, {"nested_records", true}, {"nullable_values", true},
        {"ordinary_throwing_paths_preserved", true}, {"unsafe_defaults_and_reads_refused", true},
        {"exact_header_and_compiled_modes", true}, {"active_host_modes", true},
        {"lean_constexpr_headers", true}, {"allocation_trapped_profiles", profileCount}, {"checks_per_consumer", 288 * profileCount}}));
}
