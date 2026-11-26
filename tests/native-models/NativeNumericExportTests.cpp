// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericExportTests.hpp"
#include "NativeNumericRecordFixture.hpp"
#include "NativeNumericResultFixture.hpp"
#include "NativeStructuredValueFixture.hpp"
#include "NativeFixedArrayFixture.hpp"
#include "NativeForLoopFixture.hpp"
#include "NativeNumericArrayFixture.hpp"
#include "NativeNumericFloatingFixture.hpp"
#include "NativeNumericEnvironmentFixture.hpp"
#include "NativeNumericCountedArrayFixture.hpp"
#include "NativeNumericIntegerFixture.hpp"
#include "NativeObservationFixture.hpp"
#include "NativeActiveNumericFixture.hpp"
#include "NativeByteTextFixture.hpp"
#include "NativeNumericHeaderFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include "../../src/native/sdk-tools/bindings.hpp"

#include <set>

namespace {
using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command)
{
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "native numeric export consumer failed: " + result.output + result.diagnostic);
}
}

void nativeNumericExportContracts(const std::filesystem::path& prefix,
                                  const std::filesystem::path& output)
{
    require(!fs::exists(output), "numeric export proof output must be fresh");
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Operations.pipe", R"pipe(module Numeric;
public model Class Operations {
    public pure float32 Blend(float32 first, float32 second, float32 weight)
        => first + (second - first) * weight;
    public pure float Clip(float value) {
        float result = value;
        if (result < 0.0)
            result = 0.0;
        return result > 1.0 ? 1.0 : result;
    }
    public pure bool Finite(float32 value) => float32.IsFinite(value);
    public pure float32 Signed(float32 magnitude, float32 sign)
        => float32.CopySign(float32.Abs(magnitude), sign);
    public pure float32 Forward(float32 value) => Blend(value, value, 0.5f);
    public pure float32 Loop(float32 value) {
        int index = 0;
        while (index < 2) {
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }
    public pure float32 Unwrap(float32? value) => value.Value;
    public pure string Text(string value) => value;
}
public model Class Policy {
    public pure float32 Policy(float32 value) => value;
    public pure float Policy(float value) => value;
}
public model Class Stateful {
    public float32 value;
    public pure float32 Read() => value;
}
public model Class Constructed {
    public Constructed() {
        if (float32.IsFinite(1.0f)) {
        }
    }
    public pure float32 Read(float32 value) => value;
}
)pipe");
    writeNativeNumericRecordFixture(output);
    writeNativeNumericResultFixture(output);
    writeNativeNumericArrayFixture(output);
    writeNativeNumericFloatingFixture(output);
    writeNativeNumericEnvironmentFixture(output);
    writeNativeNumericCountedArrayFixture(output);
    writeNativeNumericIntegerFixture(output);
    writeNativeByteTextFixture(output);
    const auto project = SourceProject::load(output, "src", "Operations.pipe");
    const auto request = project.request("numeric_fixture");
    const auto compiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                  request.dump(), std::chrono::seconds(45), 4U << 20);
    require(compiled.exit == 0, "numeric export compiler transport failed");
    const auto response = parseJson(compiled.output, 4U << 20);
    require(response.at("ok") == true, "numeric export source failed: " + response.dump());
    std::set<std::string> methods;
    std::size_t collisionOverloads = 0;
    for (const auto& item : response.at("manifest").at("nativeNumericExports")) {
        require(item.at("allocationFree") == true && item.at("noexcept") == true,
                "numeric export guarantees changed");
        if (item.at("owner") == "Numeric.ByteText" || item.at("owner") == "Numeric.RecordQueries" || item.at("owner") == "Numeric.ResultQueries" ||
            item.at("owner") == "Numeric.ClosedQueries" || item.at("owner") == "Numeric.CompositionQueries" ||
            item.at("owner") == "Numeric.ArrayQueries" || item.at("owner") == "Numeric.ArrayMath" ||
            item.at("owner") == "Numeric.StatefulArrayMath" || item.at("owner") == "Numeric.OrdinaryArrayCaller" ||
            item.at("owner") == "Numeric.FloatingQueries" || item.at("owner") == "Numeric.CountedArrayQueries" ||
            item.at("owner") == "Numeric.IntegerQueries" || item.at("owner") == "Numeric.EnvironmentQueries" ||
            item.at("owner") == "Numeric.WideEnvironmentMath" || item.at("owner") == "Numeric.MixedEnvironmentMath")
            continue;
        if (item.at("owner") == "Numeric.Policy") {
            require(item.at("method") == "Policy" && item.at("parameters").size() == 1 &&
                    item.at("parameters")[0].at("type") == item.at("returnType"),
                    "class/member collision changed the authored signature");
            ++collisionOverloads;
        } else {
            require(item.at("owner") == "Numeric.Operations", "unsafe owner admitted");
            methods.insert(item.at("method").get<std::string>());
        }
    }
    require(methods == std::set<std::string>{"Blend", "Clip", "Finite", "Forward", "Loop", "Signed"},
            "numeric export proof admitted state, ownership, calls or unsupported values");
    require(collisionOverloads == 2, "same-name numeric overload exports were lost");
    verifyNativeNumericRecordExports(response.at("manifest"));
    verifyNativeNumericResultExports(response.at("manifest"));
    verifyNativeNumericArrayExports(response.at("manifest"));
    verifyNativeNumericFloatingExports(response.at("manifest"));
    verifyNativeNumericEnvironmentExports(response.at("manifest"));
    verifyNativeNumericCountedArrayExports(response.at("manifest"));
    verifyNativeNumericIntegerExports(response.at("manifest"));
    verifyNativeByteTextExports(response.at("manifest"));
    verifyNativeNumericCountedArrayRejections(prefix, request);
    verifyNativeNumericArrayRejections(prefix, request);
    admitModel(response.at("manifest"), "numeric_fixture");
    for (const auto& field : {"owner", "returnType", "allocationFree", "noexcept", "binary32Environment"}) {
        for (const bool remove : {false, true}) {
            auto invalid = response.at("manifest");
            auto& exported = invalid["nativeNumericExports"][0];
            if (remove) {
                exported.erase(field);
            } else if (std::string(field) == "allocationFree" || std::string(field) == "noexcept") {
                exported[field] = false;
            } else {
                exported[field] = "invalid";
            }
            bool refused = false;
            try {
                admitModel(invalid, "numeric_fixture");
            } catch (const std::exception&) {
                refused = true;
            }
            require(refused, "numeric export schema admitted missing or corrupt metadata: " + std::string(field));
        }
    }
    auto recursive = request;
    recursive["modules"][0]["source"] = recursive["modules"][0]["source"].get<std::string>() +
        "\npublic model Class Recursive { public pure float32 Read(float32 value) => Read(value); }\n";
    const auto recursion = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                  recursive.dump(), std::chrono::seconds(45), 4U << 20);
    require(recursion.exit == 0, "recursive numeric request failed to execute");
    const auto recursiveResponse = parseJson(recursion.output, 4U << 20);
    require(recursiveResponse.at("ok") == true, "bounded recursive numeric source was refused");
    for (const auto& item : recursiveResponse.at("manifest").at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.Recursive",
                "recursive numeric closure escaped through an unguarded native export");
    }
    for (const auto& invalidBody : {
        "public pure uint32 Read(uint32 value) => float32.ToBits(value);",
        "public pure float32 Read(float32 value) => float32.FromBits(value);",
        "public pure uint32 Read(float value) => float.ToBits(value);",
        "public pure Result<uint32,uint32> Read(uint32 value) => Result<uint32,uint32>.Failure(value);",
        "public async Task<Result<uint32,ClosedError>> Read(uint32 value) { return Result<uint32,ClosedError>.Success(value); }",
        "public pure uint32 Read(Result<uint32,ClosedError> result) { if (result.IsSuccess) { Result<uint32,ClosedError> result = Result<uint32,ClosedError>.Failure(ClosedError.Invalid); return result.Value; } return 0u; }"}) {
        auto invalidBits = request;
        invalidBits["modules"][0]["source"] = invalidBits["modules"][0]["source"].get<std::string>() +
            "\npublic model Class InvalidBits { " + invalidBody + " }\n";
        const auto rejected = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                      invalidBits.dump(), std::chrono::seconds(45), 4U << 20);
        require(rejected.exit == 0 && parseJson(rejected.output, 4U << 20).at("ok") == false,
                "numeric capability admitted an invalid primitive or unsupported Result boundary: " +
                    std::string(invalidBody));
    }
    auto ordinaryResult = request;
    ordinaryResult["modules"][0]["source"] = ordinaryResult["modules"][0]["source"].get<std::string>() +
        "\npublic model Class InvalidBits { "
        "public Result<uint32,ClosedError> Read(uint32 value) => Result<uint32,ClosedError>.Success(value); }\n";
    const auto ordinaryCompiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                         ordinaryResult.dump(), std::chrono::seconds(45), 4U << 20);
    require(ordinaryCompiled.exit == 0, "ordinary Result method compiler transport failed");
    const auto ordinaryResponse = parseJson(ordinaryCompiled.output, 4U << 20);
    require(ordinaryResponse.at("ok") == true, "valid ordinary Result method was rejected");
    for (const auto& item : ordinaryResponse.at("manifest").at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.InvalidBits",
                "non-pure Result method escaped through a pure native numeric export");
    }
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    generateProjectBindings(prefix, manifest, fileHash(manifest),
                            nativeExecutable(prefix / "bin/pipelang-modelc"), output, "src", "Operations.pipe",
                            "numeric_fixture", output / "generated");
    const auto compiledInformation = compiledProjectInformation(prefix, manifest, fileHash(manifest),
        nativeExecutable(prefix / "bin/pipelang-modelc"), output, "src", "Operations.pipe", "numeric_compiled_fixture");
    generateCompiledProjectBindings(prefix, manifest, fileHash(manifest),
        nativeExecutable(prefix / "bin/pipelang-modelc"), output, "src", "Operations.pipe",
        "numeric_compiled_fixture", output / "generated/compiled", compiledInformation.at("plan_sha256").get<std::string>());
    nativeObservationContracts(prefix, output / "observation");
    nativeActiveNumericContracts(prefix, output / "active");
    nativeNumericHeaderContracts(prefix, output / "header-project");
    nativeStructuredValueContracts(prefix, output / "structured-values");
    nativeFixedArrayContracts(prefix, output / "fixed-arrays");
    nativeForLoopContracts(prefix, output / "for-loops");
    atomicWrite(output / "main.cpp", R"cpp(#include "Model.hpp"
#include "compiled/Model.hpp"
#include "../header-project/default/Model.hpp"
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
#include <cstdlib>
#include <new>
#include <limits>

using Numeric = numeric_fixture::native_numeric::Numeric::Operations;
using HeaderPolicy = numeric_fixture::native_numeric::Numeric::Policy;
using CompiledPolicy = numeric_compiled_fixture::native_numeric::Numeric::Policy;
static_assert(noexcept(HeaderPolicy::Policy(1.0f)));
static_assert(noexcept(HeaderPolicy::Policy(1.0)));
static_assert(noexcept(CompiledPolicy::Policy(1.0f)));
static_assert(noexcept(CompiledPolicy::Policy(1.0)));
static_assert(noexcept(Numeric::Blend(0.0f, 1.0f, 0.5f)));
static_assert(noexcept(Numeric::Clip(0.5)));
static_assert(noexcept(Numeric::Finite(0.0f)));
static_assert(noexcept(Numeric::Signed(0.0f, -1.0f)));

// Any allocation during a direct numeric call is a contract failure, including
// the first call. No model, task scope or thread-local warmup is constructed.
void* operator new(std::size_t)
{
    std::abort();
}
void* operator new[](std::size_t)
{
    std::abort();
}
void operator delete(void* value) noexcept
{
    std::free(value);
}
void operator delete[](void* value) noexcept
{
    std::free(value);
}
void operator delete(void* value, std::size_t) noexcept
{
    std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept
{
    std::free(value);
}

)cpp" + nativeNumericRecordConsumer() + nativeNumericResultConsumer() + nativeNumericArrayConsumer() + nativeNumericFloatingConsumer() + nativeNumericEnvironmentConsumer() + nativeNumericCountedArrayConsumer() + nativeNumericIntegerConsumer() + nativeObservationConsumer() + nativeByteTextConsumer() + nativeNumericHeaderConsumer() + R"cpp(
int main()
{
    pipelang::model::requireBinary32Environment();
    if (!verifyNumericRecords() || !verifyNumericResults() || !verifyNumericArrays() || !verifyNumericFloating() ||
        !verifyNumericCountedArrays() || !verifyNumericIntegers() || !verifyNumericEnvironment() || !verifyNativeObservations() || !verifyNativeByteText() || !verifyNumericHeaders())
        return 6;
    // Exercise the authored qualification directly, plus aliases and overloads.
    // The allocation trap also covers the first call in both project modes.
    if (numeric_fixture::native_numeric::Numeric::Policy::Policy(2.0f) != 2.0f ||
        HeaderPolicy::Policy(3.0) != 3.0 ||
        numeric_compiled_fixture::native_numeric::Numeric::Policy::Policy(4.0f) != 4.0f ||
        CompiledPolicy::Policy(5.0) != 5.0)
        return 5;
    if (Numeric::Loop(2.0f) != 4.0f ||
        numeric_compiled_fixture::native_numeric::Numeric::Operations::Loop(2.0f) != 4.0f)
        return 7;
    if (Numeric::Blend(2.0f, 6.0f, 0.25f) != 3.0f || Numeric::Clip(-2.0) != 0.0 ||
        Numeric::Clip(0.5) != 0.5 || Numeric::Clip(2.0) != 1.0)
        return 1;
    if (!Numeric::Finite(1.0f) || Numeric::Finite(std::numeric_limits<float>::infinity()))
        return 2;
    if (std::bit_cast<std::uint32_t>(Numeric::Signed(0.0f, -0.0f)) != 0x80000000U)
        return 3;
    const float first = std::bit_cast<float>(0x3f800001U);
    const float second = std::bit_cast<float>(0x3f800003U);
    volatile float difference = second - first;
    volatile float scaled = difference * 0.25f;
    volatile float expected = first + scaled;
    return std::bit_cast<std::uint32_t>(Numeric::Blend(first, second, 0.25f)) ==
        std::bit_cast<std::uint32_t>(float(expected)) ? 0 : 4;
}
)cpp");
    const auto compiler = nativeTool("c++").string();
    const std::vector<std::string> common{compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-I" + (prefix / "include").string(), "-I" + (output / "generated").string(),
        (output / "main.cpp").string()};
    auto normal = common;
#ifdef _WIN32
    // Match the installed Windows SDK's existing static runtime profile.
    normal.push_back("-static");
#endif
    normal.insert(normal.end(), {"-O2", "-ffp-contract=fast", "-o", nativeExecutable(output / "normal").string()});
    invoke(normal);
    invoke({nativeExecutable(output / "normal").string()});
    verifyNativeNumericArrayOrdinaryFixture(prefix, output / "ordinary-project");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"direct_numeric_exports", methods}, {"normal", true}, {"allocation_free_first_call", true},
        {"class_member_collision_overloads", collisionOverloads}, {"header_and_compiled_project_exports", true},
        {"numeric_records_and_private_helpers", true}, {"closed_typed_results", true},
        {"fixed_local_array_numeric_projection", true}, {"floating_conversion_and_math_projection", true}, {"binary32_environment_closure_metadata", true}, {"counted_loops_and_private_fixed_array_channels", true}, {"guarded_int64_numeric_projection", true}, {"effectful_current_thread_observation", true}, {"ordinary_qualified_helper_with_sync_host", true},
        {"exact_binary32_bit_reinterpretation", true}, {"raw_bit_environment_preservation", true},
        {"floating_comparison_short_circuit_flags", true},
        {"throwing_integer_and_unproven_result_value_refused", true},
        {"guarded_result_composition_header_and_compiled", true}}));
}
