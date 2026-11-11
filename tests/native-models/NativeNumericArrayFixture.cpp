// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericArrayFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericArrayFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/ArrayQueries.pipe", R"pipe(module Numeric;
public Interface UnusedArrayHost { pure float32 Read(float32 value); }
public model Class OrdinaryArrayCaller {
    private float32 marker = 0.0f;
    public pure float32 Read(float32 value) => ArrayMath.Scale(value) + marker;
}
public model Class ArrayMath {
    public pure float32 Scale(float32 value) => value * 2.0f;
    public pure float32 Unsafe(int index) {
        Array<float32> values = Array<float32>.Create(3, 1.0f);
        return values.At(index);
    }
}
public model Class StatefulArrayMath {
    private float32 scale = 2.0f;
    public pure float32 Scale(float32 value) => value * scale;
}
public model Class ArrayQueries {
    public pure float32 Indexed(float32 value) {
        Array<float32> values = Array<float32>.Create(3, 0.0f);
        int index = 1;
        values.Set(index, value);
        index = (index + 1) % 3;
        values.Set(index, value + 1.0f);
        return values.At(1) + values.At(index);
    }

    public pure float32 MaximumExtent(float32 value) {
        Array<float32> values = Array<float32>.Create(4096, value);
        return values.At(0);
    }

    public pure float32 AboveMaximumExtent(float32 value) {
        Array<float32> values = Array<float32>.Create(4097, value);
        return values.At(0);
    }

    public pure float32 FarAboveMaximumExtent(float32 value) {
        Array<float32> values = Array<float32>.Create(5000, value);
        return values.At(0);
    }

    public pure float32 RangeArithmetic(float32 value, bool select) {
        Array<float32> values = Array<float32>.Create(9, 0.0f);
        int row = 0;
        if (select)
            row = 2;
        int column = (row + 1) % 3;
        int index = row * 3 + column;
        if (index >= 0 && index < 9) {
            values.Set(index, value);
            return values.At(index);
        }
        return 0.0f;
    }

    public pure float32 Fold(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        float32 total = 0.0f;
        foreach (float32 item in values) {
            total = total + item;
        }
        return total;
    }

    public pure float32 Joined(float32 value, bool select) {
        Array<float32> values = Array<float32>.Create(3, value);
        values.Set(2, value + 2.0f);
        int index = 0;
        if (select) {
            index = 2;
        } else {
            index = 1;
        }
        return values.At(index);
    }

    public pure float32 Scoped(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        {
            int index = 2;
            values.Set(index, value + 2.0f);
        }
        int index = 0;
        return values.At(index);
    }

    public pure float32 Snapshot(float32 value) {
        Array<float32> values = Array<float32>.Create(3, 1.0f);
        Array<float32> alias = values;
        values.Set(0, value);
        return alias.At(0) + values.At(0);
    }

    public pure Result<float32, ClosedError> Forward(Result<float32, ClosedError> result) {
        if (!result.IsSuccess)
            return result;
        Array<float32> values = Array<float32>.Create(2, result.Value);
        return Result<float32, ClosedError>.Success(values.At(0) + values.At(1));
    }

    public pure float32 Helper(float32 value) => ArrayMath.Scale(value);

    public pure float32 DynamicIndex(float32 value, int index) {
        Array<float32> values = Array<float32>.Create(3, value);
        return values.At(index);
    }
    public pure float32 DynamicLength(float32 value, int count) {
        Array<float32> values = Array<float32>.Create(count, value);
        return values.At(0);
    }
    public pure float32 OutOfBounds(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        return values.At(3);
    }
    public pure float32 NegativeIndex(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        return values.At(-1);
    }
    public pure float32 Overflow(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        int index = 9223372036854775807;
        index = index + 1;
        return values.At(index);
    }
    public pure float32 ZeroDivisor(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        int divisor = 0;
        int index = 1 % divisor;
        return values.At(index);
    }
    public pure float32 ReassignArray(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        values = Array<float32>.Create(3, 1.0f);
        return values.At(0);
    }
    public pure float32 UnsafeJoin(float32 value, bool select) {
        Array<float32> values = Array<float32>.Create(3, value);
        int index = 0;
        if (select)
            index = 3;
        return values.At(index);
    }
    public pure float32 UnsafeScope(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        {
            int index = 0;
            float32 ignored = values.At(index);
            values.Set(index, ignored);
        }
        int index = 3;
        return values.At(index);
    }
    public pure float32 MutatingSnapshot(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        float32 total = 0.0f;
        foreach (float32 item in values) {
            total = total + item;
            values.Set(2, item + 1.0f);
        }
        return total + values.At(2);
    }
    public pure float32 ArrayParameter(Array<float32> values) => values.At(0);
    public pure Array<float32> Escaping(float32 value)
        => Array<float32>.Create(3, value);
    public ArrayMath EscapingModel() => new ArrayMath();
    public pure float32 UnsafeHelper(float32 value) => ArrayMath.Unsafe(0) + value;
    public float32 StatefulHelper(float32 value) {
        StatefulArrayMath helper = new StatefulArrayMath();
        return helper.Scale(value);
    }
    public pure float32 Unbounded(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        while (value > 0.0f) {
            value = value - values.At(0);
        }
        return value;
    }
    public pure float32 BreakLoop(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        foreach (float32 item in values) {
            if (item > 0.0f)
                break;
        }
        return value;
    }
    public pure float32 ContinueLoop(float32 value) {
        Array<float32> values = Array<float32>.Create(3, value);
        foreach (float32 item in values) {
            if (item > 0.0f)
                continue;
        }
        return value;
    }
    public pure float32 WorkCap(float32 value) {
        Array<float32> outer = Array<float32>.Create(4096, value);
        float32 total = 0.0f;
        foreach (float32 first in outer) {
            foreach (float32 second in outer) {
                total = total + first + second;
            }
        }
        return total;
    }
}
)pipe");
}

void verifyNativeNumericArrayExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> queries;
    std::set<std::string> helpers;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.StatefulArrayMath" && item.at("owner") != "Numeric.OrdinaryArrayCaller",
                "stateful array helper admitted");
        if (item.at("owner") == "Numeric.ArrayQueries")
            queries.insert(item.at("method").get<std::string>());
        if (item.at("owner") == "Numeric.ArrayMath")
            helpers.insert(item.at("method").get<std::string>());
    }
    require(queries == std::set<std::string>{"Indexed", "MaximumExtent", "RangeArithmetic", "Fold", "Joined", "Scoped", "Snapshot", "Forward", "Helper", "MutatingSnapshot"},
            "numeric array exports lost a safe query or admitted an unsafe index, escape, helper or loop");
    require(helpers == std::set<std::string>{"Scale"}, "cross-owner helpers were not checked independently");
}

std::string nativeNumericArrayConsumer()
{
    return R"cpp(
template<class Query>
bool verifyArrayProjection()
{
    static_assert(noexcept(Query::Indexed(1.0f)));
    static_assert(noexcept(Query::MaximumExtent(1.0f)));
    static_assert(noexcept(Query::RangeArithmetic(1.0f, true)));
    static_assert(noexcept(Query::Fold(1.0f)));
    static_assert(noexcept(Query::Joined(1.0f, true)));
    static_assert(noexcept(Query::Scoped(1.0f)));
    static_assert(noexcept(Query::Snapshot(1.0f)));
    static_assert(noexcept(Query::Helper(1.0f)));
    static_assert(noexcept(Query::MutatingSnapshot(1.0f)));
    using Result = decltype(Query::Forward({}));
    using Error = std::remove_cvref_t<decltype(std::declval<Result>().error())>;
    static_assert(noexcept(Query::Forward(std::declval<Result>())));
    for (const float value : {-2.0f, 0.0f, 1.0f, 8.0f}) {
        if (Query::Indexed(value) != value + value + 1.0f || Query::Fold(value) != value * 3.0f ||
            Query::MaximumExtent(value) != value || Query::RangeArithmetic(value, false) != value || Query::RangeArithmetic(value, true) != value ||
            Query::Joined(value, true) != value + 2.0f || Query::Joined(value, false) != value ||
            Query::Scoped(value) != value || Query::Snapshot(value) != value + 1.0f ||
            Query::Helper(value) != value * 2.0f ||
            Query::MutatingSnapshot(value) != value * 4.0f + 1.0f)
            return false;
        const auto result = Query::Forward(Result::Success(value));
        if (!result.is_success() || result.value() != value + value)
            return false;
    }
    const auto failure = Query::Forward(Result::Failure(Error::Invalid));
    const auto cancellation = Query::Forward(Result::Cancelled(Error::Interrupted));
    return failure.is_failure() && failure.error() == Error::Invalid &&
        cancellation.is_cancelled() && cancellation.error() == Error::Interrupted;
}

bool verifyNumericArrays()
{
    return verifyArrayProjection<numeric_fixture::native_numeric::Numeric::ArrayQueries>() &&
        verifyArrayProjection<numeric_compiled_fixture::native_numeric::Numeric::ArrayQueries>();
}
)cpp";
}

void verifyNativeNumericArrayRejections(const std::filesystem::path& prefix,
                                       const pipelang::tools::Json& request)
{
    for (const auto& declaration : {
        "public model Class IneligibleHelper { private float32 state; public pure float32 Scale(float32 value) => value; }",
        "public model Class IneligibleHelper { public IneligibleHelper() { if (true) { } } public pure float32 Scale(float32 value) => value; }",
        "public model Class IneligibleHelper { public float32 Scale(float32 value) => value; }"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() + "\n" + declaration +
            "\npublic model Class InvalidArrayCaller { public pure float32 Read(float32 value) => IneligibleHelper.Scale(value); }\n";
        const auto compiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                      invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(compiled.exit == 0 && parseJson(compiled.output, 4U << 20).at("ok") == false,
                "qualified numeric helper admitted state, constructor policy or effects");
    }
    for (const auto& workerExpression : {"ArrayMath.Scale(input)", "Scale(input)"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() +
            "\npublic Class InvalidParallelCaller {"
            " private pure float32 Scale(float32 value) => ArrayMath.Scale(value);"
            " public void Run(List<float32> inputs) {"
            " parallel foreach (float32 input in inputs) { float32 result = " + workerExpression + "; } } }\n";
        const auto compiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                      invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(compiled.exit == 0 && parseJson(compiled.output, 4U << 20).at("ok") == false,
                "parallel worker admitted a direct or transitive qualified helper call");
    }
    std::string costly = "\npublic model Class QualifiedCost0 { public pure float32 Read(float32 value) => value; }\n";
    for (int level = 1; level <= 16; ++level) {
        const auto previous = "QualifiedCost" + std::to_string(level - 1) + ".Read(value)";
        costly += "public model Class QualifiedCost" + std::to_string(level) +
            " { public pure float32 Read(float32 value) => " + previous + " + " + previous + "; }\n";
    }
    auto invalid = request;
    invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() + costly;
    const auto compiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                  invalid.dump(), std::chrono::seconds(45), 4U << 20);
    require(compiled.exit == 0, "qualified helper work-limit compiler transport failed");
    const auto response = parseJson(compiled.output, 4U << 20);
    require(response.at("ok") == false &&
            response.dump().find("expanded method evaluation work limit exceeded") != std::string::npos,
            "qualified helper DAG escaped the ordinary expanded-work limit");
}

std::string nativeNumericArrayOrdinaryConsumer()
{
    return R"cpp(#include "Model.hpp"
#include "compiled/Model.hpp"

int main()
{
    pipelang::model::requireBinary32Environment();
    numeric_fixture::source::Numeric::OrdinaryArrayCaller header;
    numeric_compiled_fixture::source::Numeric::OrdinaryArrayCaller compiled;
    for (const float value : {-2.0f, 0.0f, 1.0f, 8.0f}) {
        if (header.Read(value) != value * 2.0f || compiled.Read(value) != value * 2.0f)
            return 1;
    }
    return 0;
}
)cpp";
}

void verifyNativeNumericArrayOrdinaryFixture(const std::filesystem::path& prefix,
                                            const std::filesystem::path& output)
{
    require(!fs::exists(output), "ordinary qualified helper proof output must be fresh");
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Entry.pipe", R"pipe(module Numeric;
public Interface UnusedArrayHost { pure float32 Read(float32 value); }
public model Class ArrayMath {
    public pure float32 Scale(float32 value) => value * 2.0f;
}
public model Class OrdinaryArrayCaller {
    private float32 marker = 0.0f;
    public pure float32 Read(float32 value) => ArrayMath.Scale(value) + marker;
}
)pipe");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto digest = fileHash(manifest);
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    generateProjectBindings(prefix, manifest, digest, compiler, output, "src", "Entry.pipe",
                            "numeric_fixture", output / "generated");
    const auto information = compiledProjectInformation(prefix, manifest, digest, compiler,
        output, "src", "Entry.pipe", "numeric_compiled_fixture");
    generateCompiledProjectBindings(prefix, manifest, digest, compiler, output, "src", "Entry.pipe",
        "numeric_compiled_fixture", output / "generated/compiled", information.at("plan_sha256").get<std::string>());
    atomicWrite(output / "main.cpp", nativeNumericArrayOrdinaryConsumer());
    std::vector<std::string> command{nativeTool("c++").string(), "-std=c++20", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-O2", "-I" + (prefix / "include").string(),
        "-I" + (output / "generated").string(), (output / "main.cpp").string()};
    std::size_t units = 0;
    for (const auto& file : information.at("implementation")) {
        if (file.at("kind") == "implementation") {
            command.push_back((output / "generated/compiled" / file.at("name").get<std::string>()).string());
            ++units;
        }
    }
    require(units > 0, "ordinary qualified helper plan has no implementation units");
#ifdef _WIN32
    command.push_back("-static");
#endif
    command.insert(command.end(), {"-o", nativeExecutable(output / "normal").string()});
    const auto built = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(built.exit == 0, "ordinary qualified helper consumer build failed: " + built.output + built.diagnostic);
    const auto executed = process({nativeExecutable(output / "normal").string()}, {},
                                  std::chrono::seconds(90), 4U << 20);
    require(executed.exit == 0, "ordinary qualified helper consumer failed: " + executed.output + executed.diagnostic);
}
