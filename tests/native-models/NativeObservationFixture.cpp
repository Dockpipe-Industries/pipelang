// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeObservationFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"

#include <set>

namespace {
using namespace pipelang::tools;

void writeOrdinaryObservationFixture(const std::filesystem::path& output, const Json& plan);

Json compileObservation(const std::filesystem::path& prefix, const Json& request)
{
    const auto result = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                request.dump(), std::chrono::seconds(45), 4U << 20);
    require(result.exit == 0, "native observation compiler transport failed: " + result.diagnostic);
    return parseJson(result.output, 4U << 20);
}

void verifyObservationManifest(const Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeObservationExports")) {
        require(item.at("owner") == "Observation.Queries" || item.at("owner") == "Observation.EffectOnlyQueries",
                "stateful observation owner admitted");
        require(item.at("allocationFree") == true && item.at("noexcept") == true &&
                item.at("binary32Environment") == "not-required" &&
                item.at("effects") == "current-thread-observation",
                "native observation guarantee or effect metadata changed");
        methods.insert(item.at("owner").get<std::string>() + "." + item.at("method").get<std::string>());
    }
    require(methods == std::set<std::string>{"Observation.Queries.Capture", "Observation.Queries.Supported",
        "Observation.Queries.ViaHelper", "Observation.Queries.PortableNearest", "Observation.Queries.Binary32Nearest",
        "Observation.Queries.GradualInputs", "Observation.Queries.GradualResults", "Observation.EffectOnlyQueries.Supported"},
            "native observation projection lost a safe method or admitted arbitrary effects");
    std::set<std::string> numericMethods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        require(item.at("owner") == "Observation.Queries", "observation leaked into a pure owner");
        numericMethods.insert(item.at("method").get<std::string>());
    }
    require(numericMethods == std::set<std::string>{"Admit", "EchoFloat"},
            "environment observation was mislabeled as pure numeric execution");
    admitModel(manifest, "observation_fixture");
    for (const auto& field : {"allocationFree", "noexcept", "binary32Environment", "effects"}) {
        for (const bool remove : {false, true}) {
            auto invalid = manifest;
            auto& item = invalid["nativeObservationExports"][0];
            if (remove) {
                item.erase(field);
            } else if (std::string(field) == "allocationFree" || std::string(field) == "noexcept") {
                item[field] = false;
            } else {
                item[field] = "invalid";
            }
            bool refused = false;
            try {
                admitModel(invalid, "observation_fixture");
            } catch (const std::exception&) {
                refused = true;
            }
            require(refused, "observation schema admitted a missing or malformed guarantee: " + std::string(field));
        }
    }
}

void verifyObservationRejections(const std::filesystem::path& prefix, const Json& request)
{
    for (const auto& source : {
        "public model Class Invalid { public pure uint32 Read() => FloatingEnvironment.Observe(); }",
        "public model Class Invalid { private uint32 ReadState() => FloatingEnvironment.Observe(); public pure uint32 Read() => ReadState(); }",
        "public model Class Invalid { public const uint32 State = FloatingEnvironment.Observe(); }",
        "public Record Invalid { public uint32 State = FloatingEnvironment.Observe(); }",
        "public model Class Invalid { public async Task<Result<uint32, string>> Read() { return Result<uint32, string>.Success(FloatingEnvironment.Observe()); } }",
        "public model Class Invalid { public void Read(List<uint32> values) { parallel foreach (uint32 value in values) { uint32 state = FloatingEnvironment.Observe(); } } }",
        "public model Class Invalid { private uint32 ReadState() => FloatingEnvironment.Observe(); public void Read(List<uint32> values) { parallel foreach (uint32 value in values) { uint32 state = ReadState(); } } }"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() + "\n" + source + "\n";
        require(compileObservation(prefix, invalid).at("ok") == false,
                "observation admitted pure, initialization, asynchronous or parallel misuse");
    }
}
}

void nativeObservationContracts(const std::filesystem::path& prefix,
                                const std::filesystem::path& output)
{
    require(!fs::exists(output), "native observation proof output must be fresh");
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Entry.pipe", R"pipe(module Observation;
public Interface Host { Result<uint32, string> Read(); }
public Record FloatSnapshot { public float32 Value; }
public model Class Queries {
    public uint32 Capture() => FloatingEnvironment.Observe();
    private uint32 ReadState() => FloatingEnvironment.Observe();
    public uint32 ViaHelper() => ReadState();
    public bool Supported() => Admit(FloatingEnvironment.Observe());
    public pure bool Admit(uint32 state) {
        uint32 required = FloatingEnvironment.PortableNearest | FloatingEnvironment.Binary32Nearest |
            FloatingEnvironment.Binary32GradualInputs | FloatingEnvironment.Binary32GradualResults;
        return (state & required) == required;
    }
    public bool PortableNearest() {
        uint32 state = FloatingEnvironment.Observe();
        return (state & FloatingEnvironment.PortableNearest) != 0u;
    }
    public bool Binary32Nearest() {
        uint32 state = FloatingEnvironment.Observe();
        return (state & FloatingEnvironment.Binary32Nearest) != 0u;
    }
    public bool GradualInputs() {
        uint32 state = FloatingEnvironment.Observe();
        return (state & FloatingEnvironment.Binary32GradualInputs) != 0u;
    }
    public bool GradualResults() {
        uint32 state = FloatingEnvironment.Observe();
        return (state & FloatingEnvironment.Binary32GradualResults) != 0u;
    }
    public pure float32 EchoFloat(float32 value) => value;
    public uint32 FloatingChannel(float32 value) {
        uint32 state = FloatingEnvironment.Observe();
        return state ^ float32.ToBits(value);
    }
    public float FloatReturn()
        => (FloatingEnvironment.Observe() & FloatingEnvironment.PortableNearest) != 0u ? 1.0 : 0.0;
    public FloatSnapshot RecordReturn()
        => new FloatSnapshot { Value = (FloatingEnvironment.Observe() & FloatingEnvironment.PortableNearest) != 0u ? 1.0f : 0.0f };
    public uint32 CheckedAdd(uint32 value) => value + 1u;
    public async Task<Result<uint32, string>> Async() { return Result<uint32, string>.Success(1u); }
}
public model Class EffectOnlyQueries {
    private pure bool Admit(uint32 state) {
        uint32 required = FloatingEnvironment.PortableNearest | FloatingEnvironment.Binary32Nearest |
            FloatingEnvironment.Binary32GradualInputs | FloatingEnvironment.Binary32GradualResults;
        return (state & required) == required;
    }
    public bool Supported() => Admit(FloatingEnvironment.Observe());
}
public model Class HostQueries {
    private Host host;
    public HostQueries(Host supplied) { host = supplied; }
    public Result<uint32, string> HostCall() => host.Read();
}
public model Class Stateful {
    private uint32 value = 0u;
    public uint32 Capture() => FloatingEnvironment.Observe();
    public void Change() { value = 1u; }
}
)pipe");
    const auto project = SourceProject::load(output, "src", "Entry.pipe");
    const auto request = project.request("observation_fixture");
    const auto compiled = compileObservation(prefix, request);
    require(compiled.at("ok") == true, "native observation fixture failed: " + compiled.dump());
    verifyObservationManifest(compiled.at("manifest"));
    verifyObservationRejections(prefix, request);
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto digest = fileHash(manifest);
    const auto modelCompiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    generateProjectBindings(prefix, manifest, digest, modelCompiler, output, "src", "Entry.pipe",
                            "observation_fixture", output / "generated");
    const auto plan = compiledProjectInformation(prefix, manifest, digest, modelCompiler,
        output, "src", "Entry.pipe", "compiled_observation_fixture");
    generateCompiledProjectBindings(prefix, manifest, digest, modelCompiler, output, "src", "Entry.pipe",
        "compiled_observation_fixture", output / "generated/compiled", plan.at("plan_sha256").get<std::string>());
    writeOrdinaryObservationFixture(output, plan);
}

std::string nativeObservationConsumer()
{
    return R"cpp(
#include "observation/generated/Model.hpp"
#include "observation/generated/compiled/Model.hpp"
#include <cfenv>
#include <cstdint>
#include <type_traits>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <pipelang/model/floating_environment.hpp>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define PIPELANG_TEST_AARCH64_OBSERVATION 1
#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

std::uint64_t observationFpcr() noexcept
{
    std::uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
    return value;
}

std::uint64_t observationFpsr() noexcept
{
    std::uint64_t value;
    __asm__ volatile("mrs %0, fpsr" : "=r"(value) : : "memory");
    return value;
}

void setObservationFpcr(std::uint64_t value) noexcept
{
    __asm__ volatile("msr fpcr, %0\nisb" : : "r"(value) : "memory");
}

void setObservationFpsr(std::uint64_t value) noexcept
{
    __asm__ volatile("msr fpsr, %0\nisb" : : "r"(value) : "memory");
}

bool observationAlternateHandlingSupported() noexcept
{
#if defined(__APPLE__)
    int supported = 0;
    std::size_t size = sizeof(supported);
    return ::sysctlbyname("hw.optional.arm.FEAT_AFP", &supported, &size, nullptr, 0) == 0 &&
        size == sizeof(supported) && supported == 1;
#else
    // No positive OS capability evidence means no writes to AH or FIZ. Their
    // semantics are still exercised by the portable synthetic decoder corpus.
    return false;
#endif
}
#endif

struct ObservationEnvironmentGuard {
    std::fenv_t environment;
#if defined(__SSE__) || defined(_M_X64)
    unsigned control;
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    std::uint64_t control;
    std::uint64_t status;
#endif
    bool captured;

    ObservationEnvironmentGuard() noexcept
        : captured(std::fegetenv(&environment) == 0)
    {
#if defined(__SSE__) || defined(_M_X64)
        control = _mm_getcsr();
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
        control = observationFpcr();
        status = observationFpsr();
#endif
    }

    ~ObservationEnvironmentGuard() noexcept
    {
        if (captured)
            std::fesetenv(&environment);
#if defined(__SSE__) || defined(_M_X64)
        _mm_setcsr(control);
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
        setObservationFpcr(control);
        setObservationFpsr(status);
#endif
    }
};

template<class Query>
concept ExposesPrivateObservationState = requires { Query::ReadState(); };

template<class Query>
concept ExposesPrivateObservationAdmission = requires { Query::Admit(0u); };

template<class Query, class EffectOnly>
bool verifyObservationState(std::uint32_t expectedSnapshot, bool expectedPortable, bool expectedBinary32,
                            bool expectedInputs, bool expectedResults)
{
    static_assert(!ExposesPrivateObservationState<Query>);
    static_assert(!ExposesPrivateObservationAdmission<EffectOnly>);
    static_assert(noexcept(EffectOnly::Supported()));
    static_assert(std::is_same_v<decltype(Query::Capture()), std::uint32_t>);
    static_assert(noexcept(Query::Capture()));
    static_assert(noexcept(Query::ViaHelper()));
    static_assert(noexcept(Query::Supported()));
    static_assert(noexcept(Query::PortableNearest()));
    static_assert(noexcept(Query::Binary32Nearest()));
    static_assert(noexcept(Query::GradualInputs()));
    static_assert(noexcept(Query::GradualResults()));
    const int rounding = std::fegetround();
    const int exceptions = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__SSE__) || defined(_M_X64)
    const auto control = _mm_getcsr();
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    const auto control = observationFpcr();
    const auto status = observationFpsr();
#endif
    const auto first = Query::Capture();
    const auto second = Query::ViaHelper();
    if (first != expectedSnapshot || first != second || Query::PortableNearest() != expectedPortable ||
        Query::Binary32Nearest() != expectedBinary32 || Query::GradualInputs() != expectedInputs ||
        Query::GradualResults() != expectedResults ||
        Query::Supported() != (expectedPortable && expectedBinary32 && expectedInputs && expectedResults) ||
        EffectOnly::Supported() != (expectedPortable && expectedBinary32 && expectedInputs && expectedResults))
        return false;
    if (std::fegetround() != rounding || std::fetestexcept(FE_ALL_EXCEPT) != exceptions)
        return false;
#if defined(__SSE__) || defined(_M_X64)
    if (_mm_getcsr() != control)
        return false;
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    if (observationFpcr() != control || observationFpsr() != status)
        return false;
#endif
    return true;
}

std::uint32_t expectedAarch64Observation(std::uint64_t control) noexcept
{
    // AH selects alternate handling whose instruction-specific rounding and
    // underflow overrides are outside this binary32 admission profile.
    if ((control & UINT64_C(2)) != 0)
        return 0u;
    constexpr std::uint32_t roundingFacts[]{64u, 256u, 128u, 512u};
    std::uint32_t result = 32u | 1024u | 4096u | roundingFacts[(control >> 22) & 3u];
    const bool flushResults = (control & (UINT64_C(1) << 24)) != 0;
    const bool flushInputs = flushResults || (control & UINT64_C(1)) != 0;
    if (!flushInputs)
        result |= 2048u;
    if (!flushResults)
        result |= 8192u;
    return result;
}

bool verifySyntheticAarch64Observations()
{
    static_assert(noexcept(pipelang::model::floating_environment_detail::aarch64Binary32Facts(0)));
    // Raw-control decoding is synthetic evidence on every host. No ARM control
    // write or native ARM execution is implied by this portable corpus.
    for (std::uint64_t rounding = 0; rounding < 4; ++rounding) {
        for (std::uint64_t flush = 0; flush < 2; ++flush) {
            for (std::uint64_t alternate = 0; alternate < 4; ++alternate) {
                const auto control = (rounding << 22) | (flush << 24) | alternate;
                const auto expected = expectedAarch64Observation(control);
                for (const auto unrelated : {UINT64_C(0), UINT64_C(1) << 8, UINT64_C(1) << 19,
                                             UINT64_C(1) << 25, UINT64_C(1) << 63}) {
                    if (pipelang::model::floating_environment_detail::aarch64Binary32Facts(control | unrelated) != expected)
                        return false;
                }
            }
        }
    }
    return true;
}

bool observationArithmeticGuardExpected(bool portable, bool nearest, bool gradualInputs, bool gradualResults)
{
#if defined(__SSE__) || defined(_M_X64)
    return portable && nearest && gradualInputs && gradualResults;
#else
    (void)nearest;
    (void)gradualInputs;
    (void)gradualResults;
    if (!portable)
        return false;
    // This is the retained pre-migration guard's independent behavior witness.
    // AH can make Source admission unknown while this legacy probe still passes.
    // Restore every control and status bit before testing observer preservation.
    ObservationEnvironmentGuard restore;
    if (!restore.captured)
        return false;
    volatile float minimum = std::numeric_limits<float>::min();
    volatile float subnormal = std::numeric_limits<float>::denorm_min();
    volatile float half = 0.5f;
    volatile float two = 2.0f;
    const float produced = minimum * half;
    const float consumed = subnormal * two;
    return std::bit_cast<std::uint32_t>(produced) == 0x00400000u &&
        std::bit_cast<std::uint32_t>(consumed) == 0x00000002u;
#endif
}

bool verifyObservationMode(int mode, std::uint64_t controlBits, bool alternateKnown = false)
{
    ObservationEnvironmentGuard restore;
    if (!restore.captured || std::fesetround(mode) != 0 || std::feclearexcept(FE_ALL_EXCEPT) != 0 ||
        std::feraiseexcept(FE_INVALID | FE_INEXACT) != 0)
        return false;
#if defined(__SSE__) || defined(_M_X64)
    (void)alternateKnown;
    const auto previous = _mm_getcsr();
    _mm_setcsr((previous & ~0xe040u) | static_cast<unsigned>(controlBits));
    const auto selected = _mm_getcsr();
    const bool nearest = (selected & 0x6000u) == 0;
    const bool gradualInputs = (selected & 0x0040u) == 0;
    const bool gradualResults = (selected & 0x8000u) == 0;
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    const auto previous = observationFpcr();
    std::uint64_t mutableMask = (UINT64_C(3) << 22) | (UINT64_C(1) << 24);
    if (alternateKnown)
        mutableMask |= UINT64_C(3);
    const auto requested = (previous & ~mutableMask) | (controlBits & mutableMask);
    setObservationFpcr(requested);
    const auto selected = observationFpcr();
    if ((selected & mutableMask) != (requested & mutableMask))
        return false;
    const bool known = (selected & UINT64_C(2)) == 0;
    const bool nearest = known && (selected & (UINT64_C(3) << 22)) == 0;
    const bool gradualInputs = known && (selected & ((UINT64_C(1) << 24) | UINT64_C(1))) == 0;
    const bool gradualResults = known && (selected & (UINT64_C(1) << 24)) == 0;
#else
    (void)controlBits;
    (void)alternateKnown;
    const bool nearest = false;
    const bool gradualInputs = false;
    const bool gradualResults = false;
#endif
    const int portableMode = std::fegetround();
    const bool portable = portableMode == FE_TONEAREST;
    std::uint32_t expectedSnapshot = 0u;
    switch (portableMode) {
    case FE_TONEAREST:
        expectedSnapshot = 1u | 2u;
        break;
    case FE_DOWNWARD:
        expectedSnapshot = 1u | 4u;
        break;
    case FE_UPWARD:
        expectedSnapshot = 1u | 8u;
        break;
    case FE_TOWARDZERO:
        expectedSnapshot = 1u | 16u;
        break;
    default:
        break;
    }
#if defined(__SSE__) || defined(_M_X64)
    expectedSnapshot |= 32u | 1024u | 4096u;
    switch (selected & 0x6000u) {
    case 0u:
        expectedSnapshot |= 64u;
        break;
    case 0x2000u:
        expectedSnapshot |= 128u;
        break;
    case 0x4000u:
        expectedSnapshot |= 256u;
        break;
    case 0x6000u:
        expectedSnapshot |= 512u;
        break;
    }
    if (gradualInputs)
        expectedSnapshot |= 2048u;
    if (gradualResults)
        expectedSnapshot |= 8192u;
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    expectedSnapshot |= expectedAarch64Observation(selected);
#endif
    return verifyObservationState<observation_fixture::native_observation::Observation::Queries,
        observation_fixture::native_observation::Observation::EffectOnlyQueries>(
               expectedSnapshot, portable, nearest, gradualInputs, gradualResults) &&
        verifyObservationState<compiled_observation_fixture::native_observation::Observation::Queries,
            compiled_observation_fixture::native_observation::Observation::EffectOnlyQueries>(
               expectedSnapshot, portable, nearest, gradualInputs, gradualResults);
}

bool verifyNativeObservations()
{
#if defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    const bool alternateKnown = observationAlternateHandlingSupported();
    const char* requireAlternate = std::getenv("PIPELANG_TEST_REQUIRE_AFP");
    if (requireAlternate && requireAlternate[0] == '1' && requireAlternate[1] == '\0' && !alternateKnown)
        return false;
#endif
    const int originalRounding = std::fegetround();
    const int originalExceptions = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__SSE__) || defined(_M_X64)
    const auto originalControl = _mm_getcsr();
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    const auto originalControl = observationFpcr();
    const auto originalStatus = observationFpsr();
#endif
    const auto restored = [&]() noexcept {
        if (std::fegetround() != originalRounding || std::fetestexcept(FE_ALL_EXCEPT) != originalExceptions)
            return false;
#if defined(__SSE__) || defined(_M_X64)
        if (_mm_getcsr() != originalControl)
            return false;
#elif defined(PIPELANG_TEST_AARCH64_OBSERVATION)
        if (observationFpcr() != originalControl || observationFpsr() != originalStatus)
            return false;
#endif
        return true;
    };
    if (!verifySyntheticAarch64Observations() || !restored())
        return false;
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
#if defined(PIPELANG_TEST_AARCH64_OBSERVATION)
        for (std::uint64_t rounding = 0; rounding < 4; ++rounding) {
            for (std::uint64_t flush = 0; flush < 2; ++flush) {
                const std::uint64_t alternatives = alternateKnown ? 4u : 1u;
                for (std::uint64_t alternate = 0; alternate < alternatives; ++alternate) {
                    const auto control = (rounding << 22) | (flush << 24) | alternate;
                    if (!verifyObservationMode(mode, control, alternateKnown) || !restored())
                        return false;
                }
            }
        }
#else
        for (const unsigned rounding : {0u, 0x2000u, 0x4000u, 0x6000u}) {
            for (const unsigned denormals : {0u, 0x0040u, 0x8000u, 0x8040u}) {
                if (!verifyObservationMode(mode, rounding | denormals) || !restored())
                    return false;
            }
        }
#endif
    }
#if defined(PIPELANG_TEST_AARCH64_OBSERVATION)
    std::printf("observation: native_aarch64_cases=%u feat_afp=%u synthetic_decoder_cases=160\n",
                alternateKnown ? 128u : 32u, alternateKnown ? 1u : 0u);
#else
    std::printf("observation: synthetic_aarch64_decoder_cases=160\n");
#endif
    return true;
}
)cpp";
}

namespace {
void replaceObservationText(std::string& source, const std::string& before, const std::string& after)
{
    std::size_t position = 0;
    while ((position = source.find(before, position)) != std::string::npos) {
        source.replace(position, before.size(), after);
        position += after.size();
    }
}

void writeOrdinaryObservationFixture(const std::filesystem::path& output, const Json& plan)
{
    std::string shared = nativeObservationConsumer();
    replaceObservationText(shared, "observation/generated/", "generated/");
    replaceObservationText(shared, "compiled_observation_fixture::native_observation::Observation::Queries",
                           "CompiledOrdinaryObservation");
    replaceObservationText(shared, "compiled_observation_fixture::native_observation::Observation::EffectOnlyQueries",
                           "CompiledOrdinaryEffectOnly");
    replaceObservationText(shared, "observation_fixture::native_observation::Observation::Queries",
                           "HeaderOrdinaryObservation");
    replaceObservationText(shared, "observation_fixture::native_observation::Observation::EffectOnlyQueries",
                           "HeaderOrdinaryEffectOnly");
    replaceObservationText(shared, "    return verifyObservationState<",
        "    if (!verifyOrdinaryArithmeticGuards(observationArithmeticGuardExpected(portable, nearest, gradualInputs, gradualResults)))\n"
        "        return false;\n"
        "    return verifyObservationState<");
    atomicWrite(output / "ordinary-main.cpp", R"cpp(#include "generated/Model.hpp"
#include "generated/compiled/Model.hpp"
#include <cstdint>

template<class Model>
struct OrdinaryObservationAdapter {
    inline static Model* instance = nullptr;
    inline static bool failed = false;

    template<class Value, class Error>
    static Value unwrap(const pipelang::model::Result<Value, Error>& result) noexcept
    {
        if (!result.is_success()) {
            failed = true;
            return {};
        }
        return result.value();
    }

    static std::uint32_t Capture() noexcept
    {
        return unwrap(instance->Capture());
    }
    static std::uint32_t ViaHelper() noexcept
    {
        return unwrap(instance->ViaHelper());
    }
    static bool Supported() noexcept
    {
        return unwrap(instance->Supported());
    }
    static bool PortableNearest() noexcept
    {
        return unwrap(instance->PortableNearest());
    }
    static bool Binary32Nearest() noexcept
    {
        return unwrap(instance->Binary32Nearest());
    }
    static bool GradualInputs() noexcept
    {
        return unwrap(instance->GradualInputs());
    }
    static bool GradualResults() noexcept
    {
        return unwrap(instance->GradualResults());
    }
};

using HeaderOrdinaryObservation = OrdinaryObservationAdapter<observation_fixture::source::Observation::Queries>;
using CompiledOrdinaryObservation = OrdinaryObservationAdapter<compiled_observation_fixture::source::Observation::Queries>;
using HeaderOrdinaryEffectOnly = OrdinaryObservationAdapter<observation_fixture::source::Observation::EffectOnlyQueries>;
using CompiledOrdinaryEffectOnly = OrdinaryObservationAdapter<compiled_observation_fixture::source::Observation::EffectOnlyQueries>;

bool verifyOrdinaryArithmeticGuards(bool supported)
{
    const auto check = [supported](auto& model) {
        bool rejected = false;
        try {
            if (model.EchoFloat(1.0f) != 1.0f)
                return false;
        } catch (const pipelang::model::ExecutionFailure&) {
            rejected = true;
        }
        return rejected != supported;
    };
    return check(*HeaderOrdinaryObservation::instance) && check(*CompiledOrdinaryObservation::instance);
}
)cpp" + shared + R"cpp(
int main()
{
    // Ordinary models own scopes and may allocate. Construct them under the
    // default supported environment, then exercise only observer effects under
    // hostile modes. Native facade allocation guarantees have a separate trap.
    observation_fixture::source::Observation::Queries header;
    compiled_observation_fixture::source::Observation::Queries compiled;
    observation_fixture::source::Observation::EffectOnlyQueries headerEffectOnly;
    compiled_observation_fixture::source::Observation::EffectOnlyQueries compiledEffectOnly;
    HeaderOrdinaryObservation::instance = &header;
    CompiledOrdinaryObservation::instance = &compiled;
    HeaderOrdinaryEffectOnly::instance = &headerEffectOnly;
    CompiledOrdinaryEffectOnly::instance = &compiledEffectOnly;
    if (!verifyNativeObservations() || HeaderOrdinaryObservation::failed || CompiledOrdinaryObservation::failed ||
        HeaderOrdinaryEffectOnly::failed || CompiledOrdinaryEffectOnly::failed)
        return 1;
    return 0;
}
)cpp");
    std::string build = R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(OrdinaryObservationFixture LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
add_executable(ordinary_observation ordinary-main.cpp
)cmake";
    std::size_t units = 0;
    for (const auto& file : plan.at("implementation")) {
        if (file.at("kind") == "implementation") {
            build += "    \"generated/compiled/" + file.at("name").get<std::string>() + "\"\n";
            ++units;
        }
    }
    require(units > 0, "ordinary observation plan contains no implementation units");
    build += R"cmake()
target_link_libraries(ordinary_observation PRIVATE PipeLang::ModelRuntime)
option(OBSERVATION_SANITIZERS "Instrument every observer caller and implementation" OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ordinary_observation PRIVATE -Wall -Wextra -Werror)
endif()
if(OBSERVATION_SANITIZERS)
    target_compile_options(ordinary_observation PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ordinary_observation PRIVATE -fsanitize=address,undefined)
elseif(WIN32)
    target_link_options(ordinary_observation PRIVATE -static)
endif()
)cmake";
    atomicWrite(output / "CMakeLists.txt", build);
}
}
