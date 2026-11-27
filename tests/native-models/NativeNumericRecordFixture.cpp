// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericRecordFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericRecordFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/Parameters.pipe", R"pipe(module Numeric;
public Record NumericParameters {
    public float32 Scale;
    public float32 Offset;
    public bool Enabled;
}
)pipe");
    atomicWrite(output / "src/UnsafeParameters.pipe", R"pipe(module Numeric;
public Record UnsafeParameters {
    public List<float32> Values;
}
)pipe");
    atomicWrite(output / "src/RecordQueries.pipe", R"pipe(module Numeric;
public model Class RecordQueries {
    public pure float32 Evaluate(NumericParameters parameters, float32 value) {
        if (!parameters.Enabled)
            return 0.0f;
        return Limit(Apply(parameters, value));
    }

    private pure float32 Apply(NumericParameters parameters, float32 value)
        => parameters.Scale * value + parameters.Offset;

    private pure float32 Limit(float32 value) {
        if (value < 0.0f)
            return 0.0f;
        if (value > 1.0f)
            return 1.0f;
        return value;
    }

    public pure float32 Mutate(NumericParameters parameters) {
        parameters.Scale = 2.0f;
        return parameters.Scale;
    }

    public pure float32 ReadUnsafe(UnsafeParameters parameters)
        => parameters.Values.At(0);

    public pure float32 TransitiveLoop(float32 value) => Loop(value);

    private pure float32 Loop(float32 value) {
        int index = 0;
        while (index < 2) {
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }
}
)pipe");
    std::string costly = "module Numeric;\npublic Record LargeParameters {\n";
    for (int channel = 0; channel < 64; ++channel)
        costly += "    public float32 Channel" + std::to_string(channel) + ";\n";
    costly += "}\npublic model Class Costly {\n";
    costly += "    private pure float32 Level0(LargeParameters parameters, float32 value) => parameters.Channel0 + value;\n";
    for (int level = 1; level <= 9; ++level) {
        const auto previous = "Level" + std::to_string(level - 1) + "(parameters, value)";
        costly += "    private pure float32 Level" + std::to_string(level) +
            "(LargeParameters parameters, float32 value) => " + previous + " + " + previous + ";\n";
    }
    costly += "    public pure float32 Evaluate(LargeParameters parameters, float32 value) => Level9(parameters, value);\n}\n";
    atomicWrite(output / "src/Costly.pipe", costly);
}

void verifyNativeNumericRecordExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> queries;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.Costly", "helper DAG multiplied the numeric evaluation allowance");
        if (item.at("owner") != "Numeric.RecordQueries")
            continue;
        queries.insert(item.at("method").get<std::string>());
    }
    require(queries == std::set<std::string>{"Evaluate", "TransitiveLoop"},
            "typed numeric record query missing or unsafe method admitted");

    auto invalid = manifest;
    for (auto& record : invalid["records"]) {
        if (record.value("sourceName", "") == "Numeric.NumericParameters")
            record["fields"][0]["type"] = "string";
    }
    bool refused = false;
    try {
        admitModel(invalid, manifest.at("namespace").get<std::string>());
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "numeric export schema admitted a nonnumeric record channel");
}

std::string nativeNumericRecordConsumer()
{
    return R"cpp(
template<class Query>
concept ExposesPrivateLimit = requires { Query::Limit(0.0f); };

using HeaderQueries = numeric_fixture::native_numeric::Numeric::RecordQueries;
using CompiledQueries = numeric_compiled_fixture::native_numeric::Numeric::RecordQueries;
using HeaderParameters = numeric_fixture::source::Numeric::NumericParameters;
using CompiledParameters = numeric_compiled_fixture::source::Numeric::NumericParameters;
static_assert(!ExposesPrivateLimit<HeaderQueries>);
static_assert(!ExposesPrivateLimit<CompiledQueries>);
static_assert(noexcept(HeaderQueries::Evaluate(HeaderParameters{}, 1.0f)));
static_assert(noexcept(CompiledQueries::Evaluate(CompiledParameters{}, 1.0f)));
static_assert(noexcept(HeaderQueries::TransitiveLoop(1.0f)));
static_assert(noexcept(CompiledQueries::TransitiveLoop(1.0f)));

bool verifyNumericRecords()
{
    if (HeaderQueries::TransitiveLoop(2.0f) != 4.0f || CompiledQueries::TransitiveLoop(2.0f) != 4.0f)
        return false;
    HeaderParameters parameters{0.25f, 0.25f, true};
    CompiledParameters compiled{0.25f, 0.25f, true};
    if (HeaderQueries::Evaluate(parameters, 2.0f) != 0.75f ||
        CompiledQueries::Evaluate(compiled, 2.0f) != 0.75f)
        return false;
    if (HeaderQueries::Evaluate(parameters, -2.0f) != 0.0f ||
        CompiledQueries::Evaluate(compiled, 8.0f) != 1.0f)
        return false;
    if (parameters.Scale != 0.25f || compiled.Scale != 0.25f)
        return false;
    parameters.Enabled = false;
    compiled.Enabled = false;
    return HeaderQueries::Evaluate(parameters, 8.0f) == 0.0f &&
        CompiledQueries::Evaluate(compiled, 8.0f) == 0.0f;
}
)cpp";
}
