// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeFixedArrayFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include <set>
#include <utility>
#include <vector>

namespace {
using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command)
{
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "fixed array fixture failed (" + std::to_string(result.exit) + "): " + result.output + result.diagnostic);
}
}

void nativeFixedArrayContracts(const std::filesystem::path& prefix,
                              const std::filesystem::path& output)
{
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Queries.pipe", R"pipe(module FixedValues;
public Enum Code { None, Rejected, Interrupted }
public Record Item { public int Number; public float32 Marker; }
public Record Container {
    public FixedArray<int,3> Values;
    public FixedArray<Item,2> Items;
}
public Record ArrayError { public Code Code; public FixedArray<int,2> Details; }
public Record DefaultCell { public int Value = 7; }
public Record DefaultContainer { public FixedArray<FixedArray<DefaultCell,2>,2> Cells; }
public Record ElementDefaultLeaf { public Array<int> Items = Array<int>.Create(4096, 0); }
public Record ElementDefaultContainer { public FixedArray<ElementDefaultLeaf,2> Values; }
public Record ByteDefaultLeaf { public Array<string> Items = Array<string>.Create(64, "DEFAULT_TEXT_MARKER"); }
public Record ByteDefaultContainer { public FixedArray<ByteDefaultLeaf,2> Values; }
public Record ByteLimitLeaf { public string Text; }
public Record ByteLimitValue { public ByteLimitLeaf Leaf; }
public Record ThrowingDefault { public int Value = Array<int>.Create(0, 0).At(0); }
public Record EmptyDefaultContainer { public FixedArray<ThrowingDefault,0> Values; }
public model Class EmptyDefaultEffects {
    public EmptyDefaultContainer Value;
    public pure int Count() => Value.Values.Count;
}
public model Class Queries {
    public pure DefaultContainer Defaults() => new DefaultContainer { };
    public pure ElementDefaultContainer ImplicitElements() => new ElementDefaultContainer { };
    public pure ByteDefaultContainer ImplicitBytes() => new ByteDefaultContainer { };
    public pure FixedArray<ByteLimitValue,256> ByteLimited(ByteLimitValue fill)
        => FixedArray<ByteLimitValue,256>.Create(fill);
    public pure FixedArray<int,3> Integers(int fill) => FixedArray<int,3>.Create(fill);
    public pure FixedArray<float32,3> Singles(float32 fill) => FixedArray<float32,3>.Create(fill);
    public pure FixedArray<float,2> Wides(float fill) => FixedArray<float,2>.Create(fill);
    public pure FixedArray<Code,2> Codes(Code fill) => FixedArray<Code,2>.Create(fill);
    public pure FixedArray<Item,2> Items(int number, float32 marker)
        => FixedArray<Item,2>.Create(new Item { Number = number, Marker = marker });
    public pure FixedArray<FixedArray<int,2>,2> Nested(int fill)
        => FixedArray<FixedArray<int,2>,2>.Create(FixedArray<int,2>.Create(fill));
    public pure FixedArray<int?,3> OptionalElements(int fill, bool enabled) {
        int? element = enabled ? fill : null;
        return FixedArray<int?,3>.Create(element);
    }
    public pure FixedArray<int,3>? OptionalArray(int fill, bool enabled)
        => enabled ? FixedArray<int,3>.Create(fill) : null;
    public pure int ReadOptional(FixedArray<int?,3> values) {
        int? element = values.At(0);
        if (element.HasValue)
            return element.Value;
        return -1;
    }
    public pure FixedArray<int,3> Forward(FixedArray<int,3> values) => values;
    public pure FixedArray<int,3> Replace(FixedArray<int,3> values, int replacement) {
        values.Set(1, replacement);
        return values;
    }
    public pure Container Package(int fill, float32 marker) => new Container {
        Values = FixedArray<int,3>.Create(fill),
        Items = FixedArray<Item,2>.Create(new Item { Number = fill, Marker = marker })
    };
    public pure Container ForwardContainer(Container value) => value;
    public pure Result<FixedArray<float32,3>,ArrayError> Outcome(float32 marker, int state) {
        if (state < 0)
            return Result<FixedArray<float32,3>,ArrayError>.Cancelled(new ArrayError {
                Code = Code.Interrupted, Details = FixedArray<int,2>.Create(state) });
        if (state == 0)
            return Result<FixedArray<float32,3>,ArrayError>.Failure(new ArrayError {
                Code = Code.Rejected, Details = FixedArray<int,2>.Create(state) });
        return Result<FixedArray<float32,3>,ArrayError>.Success(FixedArray<float32,3>.Create(marker));
    }
    public pure Result<FixedArray<float32,3>,ArrayError> ForwardOutcome(Result<FixedArray<float32,3>,ArrayError> value) => value;
    public pure int Lengths() {
        FixedArray<int,3> values = FixedArray<int,3>.Create(1);
        return values.Length + values.Count;
    }
    public pure int ExtentLimits() {
        FixedArray<int,0> empty = FixedArray<int,0>.Create(3);
        FixedArray<int,4096> maximum = FixedArray<int,4096>.Create(7);
        return empty.Count + maximum.Length;
    }
    public pure int ReadFirst(FixedArray<int,3> values) => values.At(0);
    public pure int ReadLast(FixedArray<int,3> values) => values.At(2);
    public pure int Guarded(FixedArray<int,3> values, int index) {
        if (index < 0 || index >= 3)
            return -1;
        return values.At(index);
    }
    public pure int Counted() {
        FixedArray<int,3> values = FixedArray<int,3>.Create(0);
        int sum = 0;
        int index = 0;
        while (index < 3) {
            values.Set(index, index + 1);
            sum = sum + values.At(index);
            index = index + 1;
        }
        return sum;
    }
    public pure int Snapshot() {
        FixedArray<int,3> values = FixedArray<int,3>.Create(1);
        int sum = 0;
        foreach (int value in values) {
            values.Set(1, 9);
            sum = sum + value;
        }
        return sum;
    }
    public pure int UnsafeAt(FixedArray<int,3> values, int index) => values.At(index);
    public pure FixedArray<int,3> UnsafeSet(FixedArray<int,3> values, int index) {
        values.Set(index, 7);
        return values;
    }
    public pure int UnsafeOptional(FixedArray<int?,3> values) => values.At(0).Value;
    public pure Array<int> Dynamic(Array<int> values) => values;
}
)pipe");
    auto fixtureSource = readFile(output / "src/Queries.pipe", 4U << 20);
    const std::string defaultTextMarker = "DEFAULT_TEXT_MARKER";
    fixtureSource.replace(fixtureSource.find(defaultTextMarker), defaultTextMarker.size(), std::string(8192, 'x'));
    atomicWrite(output / "src/Queries.pipe", fixtureSource);
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto project = SourceProject::load(output, "src", "Queries.pipe");
    auto request = project.request("fixed_header");
    const auto response = process({compiler.string()}, request.dump(), std::chrono::seconds(45), 4U << 20);
    require(response.exit == 0, "fixed array compiler transport failed");
    const auto parsed = parseJson(response.output, 4U << 20);
    require(parsed.at("ok") == true, "fixed array source refused: " + parsed.dump());
    const auto& model = parsed.at("manifest");
    std::set<std::string> methods;
    for (const auto& item : model.at("nativeNumericExports")) {
        if (item.at("owner") == "FixedValues.Queries")
            methods.insert(item.at("method").get<std::string>());
    }
    const std::set<std::string> expected{"Integers", "Singles", "Wides", "Codes", "Items", "Nested",
        "OptionalElements", "OptionalArray", "ReadOptional", "Forward", "Replace", "Package", "ForwardContainer",
        "Outcome", "ForwardOutcome", "Lengths", "ExtentLimits", "ReadFirst", "ReadLast", "Guarded", "Counted", "Snapshot", "Defaults"};
    require(methods == expected, "fixed array admission lost safe shapes or admitted unsafe access: " + canonical(methods));
    admitModel(model, "fixed_header");
    for (const std::string type : {"FixedArray<int,-1>", "FixedArray<int,4097>", "FixedArray<int,three>",
             "FixedArray<int>", "FixedArray<int,3,4>", "FixedArray<List<int>,3>", "Array<int>", "FixedArray<int,256>",
             "FixedArray<FixedValues.ByteLimitValue,256>", "FixedArray<FixedArray<int,64>,64>",
             "FixedArray<FixedArray<int,0>,4096>"}) {
        auto invalid = model;
        for (auto& record : invalid["records"]) {
            if (record.value("sourceName", "") == "FixedValues.Container")
                record["fields"][0]["type"] = type;
        }
        bool refused = false;
        try {
            admitModel(invalid, "fixed_header");
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "fixed array schema admitted an invalid extent, malformed generic or dynamic element: " + type);
    }
    // A zero extent contributes no child layout nodes. Exercise this directly
    // through schema admission without adding more Source declarations/exports.
    auto zeroLayout = model;
    const auto addWideRecord = [&](const std::string& name, int fields) {
        Json declarations = Json::array();
        for (int index = 0; index < fields; ++index)
            declarations.push_back(Json{{"name", "Field" + std::to_string(index)}, {"type", "int"}});
        zeroLayout["records"].push_back(Json{{"name", name}, {"sourceName", "FixedValues." + name},
            {"fields", declarations}});
    };
    addWideRecord("WideA", 128);
    addWideRecord("WideB", 125);
    zeroLayout["records"].push_back(Json{{"name", "BoundaryRecord"}, {"sourceName", "FixedValues.BoundaryRecord"},
        {"fields", Json::array({Json{{"name", "First"}, {"type", "FixedValues.WideA"}},
            Json{{"name", "Second"}, {"type", "FixedValues.WideB"}}})}});
    for (auto& record : zeroLayout["records"]) {
        if (record.value("sourceName", "") == "FixedValues.Container")
            record["fields"][0]["type"] = "FixedArray<FixedValues.BoundaryRecord,0>";
    }
    admitModel(zeroLayout, "fixed_header");
    for (const std::string type : {"Result<FixedArray<int,-1>,FixedValues.ArrayError>",
             "Result<FixedArray<int,4097>,FixedValues.ArrayError>", "Result<FixedArray<int,3,4>,FixedValues.ArrayError>",
             "Result<FixedArray<Array<int>,3>,FixedValues.ArrayError>", "Result<FixedArray<int,3>,UnknownError>"}) {
        auto invalid = model;
        for (auto& item : invalid["nativeNumericExports"]) {
            if (item.at("owner") == "FixedValues.Queries" && item.at("method") == "Outcome")
                item["returnType"] = type;
        }
        bool refused = false;
        try {
            admitModel(invalid, "fixed_header");
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "fixed array Result schema admitted malformed nested payload/error: " + type);
    }
    for (const std::string alias : {"FixedArray<List<int>,3>", "FixedArray", "Array<int>"}) {
        auto invalid = model;
        invalid["enums"].push_back(Json{{"name", "ReservedAlias"}, {"sourceName", alias}, {"values", Json::array({"Value"})}});
        for (auto& record : invalid["records"]) {
            if (record.value("sourceName", "") == "FixedValues.Container")
                record["fields"][0]["type"] = alias;
        }
        bool refused = false;
        try {
            admitModel(invalid, "fixed_header");
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "fixed array schema accepted a reserved/malformed alias: " + alias);
    }
    for (const std::string extent : {"-1", "4097", "1.5", "unknown"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = "module FixedValues; public model Class Invalid { public pure int Length() { FixedArray<int," +
            extent + "> values = FixedArray<int," + extent + ">.Create(0); return values.Length; } }";
        const auto refused = process({compiler.string()}, invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(refused.exit == 0 && parseJson(refused.output, 4U << 20).at("ok") == false,
            "Source accepted an invalid fixed extent: " + extent);
    }
    for (const auto& [type, fill] : std::vector<std::pair<std::string, std::string>>{
             {"FixedArray<FixedArray<int,64>,64>", "FixedArray<int,64>.Create(0)"},
             {"FixedArray<FixedArray<FixedArray<int,0>,4096>,4096>", "FixedArray<FixedArray<int,0>,4096>.Create(FixedArray<int,0>.Create(0))"}}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = "module FixedValues; public model Class Invalid { public pure int Length() { " +
            type + " values = " + type + ".Create(" + fill + "); return values.Length; } }";
        const auto refused = process({compiler.string()}, invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(refused.exit == 0 && parseJson(refused.output, 4U << 20).at("ok") == false,
            "Source accepted fixed array ownership amplification beyond existing limits: " + type);
    }
    generateProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "fixed_header", output / "header");
    const auto compiledInfo = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "fixed_compiled");
    generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "fixed_compiled", output / "compiled", compiledInfo.at("plan_sha256").get<std::string>());
    generateProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "fixed_active", output / "active", "active-host");
    const auto activeInfo = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "fixed_active_compiled", "active-host");
    generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "fixed_active_compiled", output / "active-compiled", activeInfo.at("plan_sha256").get<std::string>(), "active-host");
    const auto lean = readFile(output / "header/Numeric.hpp", 4U << 20);
    require(lean.find("runtime.hpp") == std::string::npos && lean.find("object.hpp") == std::string::npos,
        "fixed array numeric header imported an interpreter runtime");
    atomicWrite(output / "checks.hpp", R"cpp(#pragma once
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
extern bool forbidFixedAllocations;
inline std::size_t fixedChecks = 0;
template<class Query>
concept ExposesUnsafeAt = requires { &Query::UnsafeAt; };
template<class Query>
concept ExposesUnsafeSet = requires { &Query::UnsafeSet; };
template<class Query>
concept ExposesUnsafeOptional = requires { &Query::UnsafeOptional; };
template<class Query>
concept ExposesDynamic = requires { &Query::Dynamic; };
template<class Query>
concept ExposesByteLimited = requires { &Query::ByteLimited; };
template<class Query>
concept ExposesImplicitElements = requires { &Query::ImplicitElements; };
template<class Query>
concept ExposesImplicitBytes = requires { &Query::ImplicitBytes; };

inline std::size_t fixedDefaultConstructions = 0;
inline std::size_t fixedDefaultValidations = 0;
inline std::size_t fixedDefaultElements = 0;
struct FixedDefaultProbe {
    std::size_t elements;
    FixedDefaultProbe() : elements(fixedDefaultElements)
    {
        ++fixedDefaultConstructions;
    }
};
inline void validate(const FixedDefaultProbe&)
{
    ++fixedDefaultValidations;
}
inline std::size_t ownedElements(const FixedDefaultProbe& value)
{
    return value.elements;
}
static_assert(pipelang::model::FixedArray<FixedDefaultProbe,0>{}.count() == 0);

struct ConstantOwnershipProbe {};
inline constexpr std::size_t ownedElements(const ConstantOwnershipProbe&)
{
    return pipelang::model::maximumListElements;
}
template<class Element, std::size_t Extent>
concept ConstantFixedDefault = requires {
    typename std::integral_constant<std::int64_t, pipelang::model::FixedArray<Element,Extent>{}.count()>;
};
static_assert(ConstantFixedDefault<ConstantOwnershipProbe,0>);
static_assert(!ConstantFixedDefault<ConstantOwnershipProbe,1>);

inline std::size_t fixedOwnedDefaultConstructions = 0;
inline std::size_t fixedOwnedDefaultLiveOwners = 0;
struct FixedOwnedDefaultProbe {
    std::unique_ptr<int> payload = std::make_unique<int>(7);

    FixedOwnedDefaultProbe()
    {
        ++fixedOwnedDefaultConstructions;
        ++fixedOwnedDefaultLiveOwners;
    }
    FixedOwnedDefaultProbe(FixedOwnedDefaultProbe&& other) noexcept
        : payload(std::move(other.payload))
    {
    }
    ~FixedOwnedDefaultProbe()
    {
        if (payload)
            --fixedOwnedDefaultLiveOwners;
    }
};
inline std::size_t ownedElements(const FixedOwnedDefaultProbe&)
{
    return pipelang::model::maximumListElements / 2;
}

inline bool verifyFixedDefaultUnwind()
{
    if (fixedOwnedDefaultLiveOwners != 0)
        return false;
    fixedOwnedDefaultConstructions = 0;
    bool rejected = false;
    try {
        const pipelang::model::FixedArray<FixedOwnedDefaultProbe,3> excessive;
        (void)excessive;
    } catch (const pipelang::model::ExecutionFailure& error) {
        rejected = std::string_view(error.what()) == "collection aggregate element limit exceeded";
    }
    return rejected && fixedOwnedDefaultConstructions == 2 && fixedOwnedDefaultLiveOwners == 0;
}

inline bool verifyFixedDefaultEffects()
{
    fixedDefaultConstructions = 0;
    fixedDefaultValidations = 0;
    fixedDefaultElements = 0;
    const pipelang::model::FixedArray<FixedDefaultProbe,0> empty;
    if (empty.count() != 0 || fixedDefaultConstructions != 0 || fixedDefaultValidations != 0)
        return false;
    const pipelang::model::FixedArray<FixedDefaultProbe,3> values;
    if (values.count() != 3 || fixedDefaultConstructions != 3 || fixedDefaultValidations != 3)
        return false;
    fixedDefaultConstructions = 0;
    fixedDefaultValidations = 0;
    fixedDefaultElements = pipelang::model::maximumListElements / 2;
    bool rejected = false;
    try {
        const pipelang::model::FixedArray<FixedDefaultProbe,3> excessive;
        (void)excessive;
    } catch (const pipelang::model::ExecutionFailure& error) {
        rejected = std::string_view(error.what()) == "collection aggregate element limit exceeded";
    }
    fixedDefaultElements = 0;
    return rejected && fixedDefaultConstructions == 2 && fixedDefaultValidations == 2;
}

template<class Query>
bool verifyImplicitDefaultFailures(Query& query)
{
    bool elementsRejected = false;
    bool bytesRejected = false;
    try {
        (void)query.ImplicitElements();
    } catch (const pipelang::model::ExecutionFailure& error) {
        elementsRejected = std::string_view(error.what()) == "collection aggregate element limit exceeded";
    }
    try {
        (void)query.ImplicitBytes();
    } catch (const pipelang::model::ExecutionFailure& error) {
        bytesRejected = std::string_view(error.what()) == "collection owned byte limit exceeded";
    }
    return elementsRejected && bytesRejected;
}

template<class Query>
bool verifyFixedArrays()
{
    static_assert(!ExposesUnsafeAt<Query> && !ExposesUnsafeSet<Query>);
    static_assert(!ExposesUnsafeOptional<Query> && !ExposesDynamic<Query> && !ExposesByteLimited<Query>);
    static_assert(!ExposesImplicitElements<Query> && !ExposesImplicitBytes<Query>);
    static_assert(noexcept(Query::Integers(1)) && noexcept(Query::Snapshot()));
    static_assert(noexcept(Query::Singles(0.f)) && noexcept(Query::Wides(0.0)));
    const auto original = Query::Integers(7);
    const auto forwarded = Query::Forward(original);
    const auto replaced = Query::Replace(original, 19);
    const auto defaults = Query::Defaults();
    if (defaults.Cells.uncheckedAt(1).uncheckedAt(1).Value != 7)
        return false;
    if (original.count() != 3 || forwarded.count() != 3 || replaced.count() != 3 || Query::Lengths() != 6
        || Query::ExtentLimits() != 4096 || Query::Counted() != 6 || Query::Snapshot() != 3)
        return false;
    for (std::size_t index = 0; index < 3; ++index) {
        if (original.uncheckedAt(index) != 7 || forwarded.uncheckedAt(index) != 7
            || replaced.uncheckedAt(index) != (index == 1 ? 19 : 7))
            return false;
    }
    auto hostCopy = original;
    hostCopy.set(0, 31);
    if (original.uncheckedAt(0) != 7 || hostCopy.uncheckedAt(0) != 31 || Query::ReadFirst(original) != 7
        || Query::ReadLast(replaced) != 7 || Query::Guarded(replaced, 1) != 19
        || Query::Guarded(original, -1) != -1 || Query::Guarded(original, 3) != -1)
        return false;
    auto hostSnapshot = original.values();
    hostSnapshot[0] = 99;
    if (original.uncheckedAt(0) != 7 || hostSnapshot[0] != 99)
        return false;
    const auto nested = Query::Nested(23);
    if (nested.count() != 2 || nested.uncheckedAt(0).count() != 2)
        return false;
    for (std::size_t outer = 0; outer < 2; ++outer)
        for (std::size_t inner = 0; inner < 2; ++inner)
            if (nested.uncheckedAt(outer).uncheckedAt(inner) != 23)
                return false;
    for (const bool enabled : {false, true}) {
        const auto elements = Query::OptionalElements(17, enabled);
        const auto optional = Query::OptionalArray(29, enabled);
        if (elements.count() != 3 || optional.has_value() != enabled
            || Query::ReadOptional(elements) != (enabled ? 17 : -1))
            return false;
        for (std::size_t index = 0; index < 3; ++index) {
            if (elements.uncheckedAt(index).has_value() != enabled)
                return false;
            if (enabled && (elements.uncheckedAt(index).value() != 17 || optional.value().uncheckedAt(index) != 29))
                return false;
        }
    }
    using Result = decltype(Query::Outcome(0.f, 0));
    using Error = std::remove_cvref_t<decltype(std::declval<Result>().error())>;
    using Code = decltype(Error{}.Code);
    for (const auto code : {Code::None, Code::Rejected, Code::Interrupted}) {
        const auto values = Query::Codes(code);
        if (values.count() != 2 || values.uncheckedAt(0) != code || values.uncheckedAt(1) != code)
            return false;
    }
    const std::array<std::uint32_t, 14> words{0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu,
        0x00800000u, 0x3f000000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
        0x7fc12345u, 0xffc12345u, 0x7f812345u};
    for (const auto bits : words) {
        const float marker = std::bit_cast<float>(bits);
        const auto scalars = Query::Singles(marker);
        const auto records = Query::Items(37, marker);
        const auto container = Query::Package(41, marker);
        const auto copied = Query::ForwardContainer(container);
        if (scalars.count() != 3 || records.count() != 2 || copied.Values.count() != 3 || copied.Items.count() != 2)
            return false;
        for (std::size_t index = 0; index < 3; ++index) {
            if (std::bit_cast<std::uint32_t>(scalars.uncheckedAt(index)) != bits || copied.Values.uncheckedAt(index) != 41)
                return false;
        }
        for (std::size_t index = 0; index < 2; ++index) {
            if (records.uncheckedAt(index).Number != 37 || copied.Items.uncheckedAt(index).Number != 41
                || std::bit_cast<std::uint32_t>(records.uncheckedAt(index).Marker) != bits
                || std::bit_cast<std::uint32_t>(copied.Items.uncheckedAt(index).Marker) != bits)
                return false;
        }
        for (const auto state : {-3, 0, 5}) {
            const auto result = Query::Outcome(marker, state);
            const auto forwardedResult = Query::ForwardOutcome(result);
            if (result.outcome() != forwardedResult.outcome())
                return false;
            if (state > 0) {
                if (!result.is_success() || result.value().count() != 3 || result.error().Code != Code::None)
                    return false;
                for (std::size_t index = 0; index < 3; ++index) {
                    if (std::bit_cast<std::uint32_t>(result.value().uncheckedAt(index)) != bits
                        || std::bit_cast<std::uint32_t>(forwardedResult.value().uncheckedAt(index)) != bits)
                        return false;
                }
            } else {
                if (result.is_cancelled() != (state < 0) || result.is_failure() != (state == 0)
                    || result.error().Code != (state < 0 ? Code::Interrupted : Code::Rejected))
                    return false;
                for (std::size_t index = 0; index < 2; ++index) {
                    if (result.error().Details.uncheckedAt(index) != state
                        || forwardedResult.error().Details.uncheckedAt(index) != state)
                        return false;
                }
            }
        }
        ++fixedChecks;
    }
    const std::array<std::uint64_t, 12> wideWords{0u, UINT64_C(0x8000000000000000), 1u,
        UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff8123456789abc), UINT64_C(0x7ff0123456789abc)};
    for (const auto bits : wideWords) {
        const auto values = Query::Wides(std::bit_cast<double>(bits));
        if (values.count() != 2 || std::bit_cast<std::uint64_t>(values.uncheckedAt(0)) != bits
            || std::bit_cast<std::uint64_t>(values.uncheckedAt(1)) != bits)
            return false;
        ++fixedChecks;
    }
    return true;
}

template<class Operation>
bool fixedFloatingProfiles(Operation operation)
{
    fenv_t initial;
    if (std::fegetenv(&initial) != 0)
        return false;
#if defined(__SSE2__)
    const unsigned initialControl = _mm_getcsr();
#endif
    bool valid = true;
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        if (std::fesetround(rounding) != 0)
            return false;
#if defined(__SSE2__)
        for (unsigned sseRounding = 0; sseRounding < 4; ++sseRounding) {
            for (unsigned denormals = 0; denormals < 4; ++denormals) {
                std::feclearexcept(FE_ALL_EXCEPT);
                std::feraiseexcept(FE_DIVBYZERO);
                const unsigned control = (_mm_getcsr() & ~0xe07fu) | (sseRounding << 13)
                    | ((denormals & 1u) ? 0x8000u : 0u) | ((denormals & 2u) ? 0x40u : 0u) | 5u;
                _mm_setcsr(control);
#else
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
#endif
                const int beforeRounding = std::fegetround();
                const int flags = std::fetestexcept(FE_ALL_EXCEPT);
#if defined(__i386__) || defined(__x86_64__)
                unsigned short beforeX87 = 0;
                asm volatile("fnstcw %0" : "=m"(beforeX87));
#endif
                forbidFixedAllocations = true;
                valid = operation() && valid;
                forbidFixedAllocations = false;
                valid = valid && std::fegetround() == beforeRounding && std::fetestexcept(FE_ALL_EXCEPT) == flags;
#if defined(__i386__) || defined(__x86_64__)
                unsigned short afterX87 = 0;
                asm volatile("fnstcw %0" : "=m"(afterX87));
                valid = valid && beforeX87 == afterX87;
#endif
#if defined(__SSE2__)
                valid = valid && _mm_getcsr() == control;
            }
        }
#endif
    }
    if (std::fesetenv(&initial) != 0)
        return false;
#if defined(__SSE2__)
    _mm_setcsr(initialControl);
#endif
    return valid;
}
)cpp");
    const std::string allocationTrap = R"cpp(
void* operator new(std::size_t bytes)
{
    if (forbidFixedAllocations)
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
    atomicWrite(output / "allocation.cpp", R"cpp(#include <cstddef>
#include <cstdlib>
#include <new>
bool forbidFixedAllocations = false;
)cpp" + allocationTrap);
    atomicWrite(output / "main.cpp", R"cpp(#include "header/Model.hpp"
#include "compiled/Model.hpp"
#include "active/Model.hpp"
#include "active-compiled/Model.hpp"
#include "checks.hpp"
int main()
{
    using Header = fixed_header::native_numeric::FixedValues::Queries;
    using Compiled = fixed_compiled::native_numeric::FixedValues::Queries;
    using Active = fixed_active::native_numeric::FixedValues::Queries;
    using ActiveCompiled = fixed_active_compiled::native_numeric::FixedValues::Queries;
    static_assert(Header::Defaults().Cells.uncheckedAt(0).uncheckedAt(1).Value == 7);
    static_assert(Compiled::Defaults().Cells.uncheckedAt(1).uncheckedAt(0).Value == 7);
    static_assert(fixed_header::source::FixedValues::DefaultContainer{}.Cells.uncheckedAt(1).uncheckedAt(1).Value == 7);
    static_assert(fixed_compiled::source::FixedValues::DefaultContainer{}.Cells.uncheckedAt(0).uncheckedAt(0).Value == 7);
    fixed_header::source::FixedValues::Queries ordinary;
    fixed_compiled::source::FixedValues::Queries compiledOrdinary;
    fixed_header::source::FixedValues::EmptyDefaultEffects emptyEffects;
    fixed_compiled::source::FixedValues::EmptyDefaultEffects compiledEmptyEffects;
    if (!verifyFixedDefaultEffects() || !verifyFixedDefaultUnwind() || !verifyImplicitDefaultFailures(ordinary)
        || !verifyImplicitDefaultFailures(compiledOrdinary) || emptyEffects.Count() != 0 || compiledEmptyEffects.Count() != 0)
        return 4;
    const auto input = ordinary.Integers(7);
    int rejected = 0;
    for (const auto index : {-1, 3}) {
        try {
            (void)ordinary.UnsafeAt(input, index);
        } catch (const pipelang::model::ExecutionFailure&) {
            ++rejected;
        }
        try {
            (void)ordinary.UnsafeSet(input, index);
        } catch (const pipelang::model::ExecutionFailure&) {
            ++rejected;
        }
    }
    try {
        (void)ordinary.UnsafeOptional(ordinary.OptionalElements(1, false));
    } catch (const pipelang::model::ExecutionFailure&) {
        ++rejected;
    }
    bool rejectedOwnedBytes = false;
    try {
        using namespace fixed_header::source::FixedValues;
        // The extent and element count are admitted. The nested text alone
        // drives the array over its owned byte budget.
        (void)ordinary.ByteLimited(ByteLimitValue{ByteLimitLeaf{std::string(8192, 'x')}});
    } catch (const pipelang::model::ExecutionFailure& error) {
        rejectedOwnedBytes = std::string_view(error.what()) == "collection owned byte limit exceeded";
    }
    if (!rejectedOwnedBytes)
        return 3;
    if (rejected != 5 || ordinary.Snapshot() != 3 || ordinary.Counted() != 6)
        return 1;
    const bool valid = fixedFloatingProfiles([] {
        return verifyFixedArrays<Header>() && verifyFixedArrays<Compiled>()
            && verifyFixedArrays<Active>() && verifyFixedArrays<ActiveCompiled>();
    });
    return valid && fixedChecks > 0 ? 0 : 2;
}
)cpp");
    atomicWrite(output / "standalone.cpp", R"cpp(#include "header/Numeric.hpp"
#include "compiled/Numeric.hpp"
#include "active/Numeric.hpp"
#include "active-compiled/Numeric.hpp"
#if defined(PIPELANG_ACTIVE_HOST_FLOATING)
#error Lean fixed array headers must not impose an active-host macro
#endif
#include "checks.hpp"
using Header = fixed_header::numeric_header::native_numeric::FixedValues::Queries;
using Compiled = fixed_compiled::numeric_header::native_numeric::FixedValues::Queries;
using Active = fixed_active::numeric_header::native_numeric::FixedValues::Queries;
using ActiveCompiled = fixed_active_compiled::numeric_header::native_numeric::FixedValues::Queries;
static_assert(Header::Integers(7).uncheckedAt(2) == 7);
static_assert(Header::ReadLast(Header::Replace(Header::Integers(7), 19)) == 7);
static_assert(Compiled::Counted() == 6 && Compiled::Snapshot() == 3);
static_assert(Header::Lengths() == 6 && Active::ExtentLimits() == 4096);
static_assert(ActiveCompiled::Nested(23).uncheckedAt(1).uncheckedAt(1) == 23);
static_assert(Header::OptionalArray(3, true).value().uncheckedAt(0) == 3);
static_assert(!Header::OptionalArray(3, false).has_value());
static_assert(Header::Defaults().Cells.uncheckedAt(0).uncheckedAt(1).Value == 7);
static_assert(Compiled::Defaults().Cells.uncheckedAt(1).uncheckedAt(0).Value == 7);
int main()
{
    if (!verifyFixedDefaultEffects() || !verifyFixedDefaultUnwind())
        return 2;
    const bool valid = fixedFloatingProfiles([] {
        return verifyFixedArrays<Header>() && verifyFixedArrays<Compiled>()
            && verifyFixedArrays<Active>() && verifyFixedArrays<ActiveCompiled>();
    });
    return valid && fixedChecks > 0 ? 0 : 1;
}
)cpp");
    atomicWrite(output / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.20)
project(FixedArrays LANGUAGES CXX)
find_package(Threads REQUIRED)
option(FIXED_ARRAY_SANITIZERS "Instrument fixed array consumers" OFF)
if(FIXED_ARRAY_SANITIZERS)
    add_compile_options(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED
    PATHS "${FIXED_ARRAY_SDK_PREFIX}/lib/cmake/PipeLangModel" NO_DEFAULT_PATH)
pipelang_add_model(fixed_compiled PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src ENTRY Queries.pipe NAMESPACE fixed_compiled
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/compiled" COMPILATION COMPILED)
add_executable(fixed_arrays main.cpp allocation.cpp)
add_executable(fixed_array_headers standalone.cpp allocation.cpp)
foreach(target fixed_arrays fixed_array_headers)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${FIXED_ARRAY_SDK_PREFIX}/include")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endforeach()
target_link_libraries(fixed_arrays PRIVATE Threads::Threads fixed_compiled)
target_compile_definitions(fixed_arrays PRIVATE PIPELANG_ACTIVE_HOST_FLOATING=1)
target_compile_options(fixed_arrays PRIVATE -frounding-math)
)cmake");
    invoke({nativeTool("cmake").string(), "-S", output.string(), "-B", (output / "initial-normal").string(),
        "-DFIXED_ARRAY_SDK_PREFIX=" + prefix.string(),
        "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
#ifdef _WIN32
        "-G", "Ninja", "-DCMAKE_EXE_LINKER_FLAGS=-static",
#endif
        "-DCMAKE_BUILD_TYPE=Release"});
    for (const auto* target : {"fixed_compiled_implementation", "fixed_arrays", "fixed_array_headers"}) {
        try {
            invoke({nativeTool("cmake").string(), "--build", (output / "initial-normal").string(),
                "--target", target, "--parallel", "1"});
        } catch (const std::exception& error) {
            throw std::runtime_error("fixed array build target " + std::string(target) + ": " + error.what());
        }
    }
    invoke({nativeExecutable(output / "initial-normal/fixed_arrays").string()});
    invoke({nativeExecutable(output / "initial-normal/fixed_array_headers").string()});
#if defined(__SSE2__)
    constexpr std::size_t profiles = 64;
#else
    constexpr std::size_t profiles = 4;
#endif
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"normal", true},
        {"fixed_extents", true}, {"zero_and_maximum_extent", true}, {"structured_result_arrays", true},
        {"nullable_and_nested_values", true}, {"copy_and_snapshot_independence", true},
        {"ordinary_bounds_throw", true}, {"unsafe_and_dynamic_exports_refused", true},
        {"malformed_schema_and_aliases_refused", true}, {"header_and_compiled_active_modes", true},
        {"standalone_constexpr_integers", true}, {"allocation_trapped_profiles", profiles}}));
}
