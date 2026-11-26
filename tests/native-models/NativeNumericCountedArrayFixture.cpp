// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericCountedArrayFixture.hpp"

#include <set>

namespace {
using namespace pipelang::tools;
}

void writeNativeNumericCountedArrayFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/CountedChannels.pipe", R"pipe(module Numeric;
public Record CountedChannels {
    public float32 X;
    public float32 Y;
    public float32 Z;
    public float32 W;
}
)pipe");
    atomicWrite(output / "src/DoubleChannels.pipe", R"pipe(module Numeric;
public Record DoubleChannels {
    public float X;
    public float Y;
}
)pipe");
    atomicWrite(output / "src/CountedArrayQueries.pipe", R"pipe(module Numeric;
public model Class CountedArrayQueries {
    public pure float32 Nested(float32 value) {
        Array<float32> components = Array<float32>.Create(16, 0.0f);
        int row = 0;
        while (row < 4) {
            int column = 0;
            while (column < 4) {
                int depth = 0;
                while (depth < 4) {
                    int index = row * 4 + column;
                    components.Set(index, components.At(index) + value);
                    depth = depth + 1;
                }
                column = column + 1;
            }
            row = row + 1;
        }
        float32 total = 0.0f;
        foreach (float32 component in components) {
            total = total + component;
        }
        return total;
    }

    public pure float32 RowsAfterPivot(float32 value) {
        Array<float32> components = Array<float32>.Create(4, value);
        float32 total = 0.0f;
        int pivot = 0;
        while (pivot < 4) {
            int row = pivot + 1;
            while (row < 4) {
                total = total + components.At(row);
                row = row + 1;
            }
            pivot = pivot + 1;
        }
        return total;
    }

    public pure float32 Stepped(float32 value) {
        Array<float32> components = Array<float32>.Create(4, value);
        float32 total = 0.0f;
        int index = 0;
        while (index < 4) {
            total = total + components.At(index);
            index = index + 2;
        }
        return total;
    }

    public pure float32 Inclusive(float32 value) {
        Array<float32> components = Array<float32>.Create(4, value);
        float32 total = 0.0f;
        int index = 0;
        while (index <= 3) {
            total = total + components.At(index);
            index = index + 1;
        }
        return total;
    }

    public pure float32 ZeroTrip(float32 value) {
        Array<float32> components = Array<float32>.Create(4, value);
        int index = 4;
        while (index < 4) {
            Array<float32> unreachable = Array<float32>.Create(4, value);
            components.Set(0, unreachable.At(index));
            index = index + 1;
        }
        return components.At(0);
    }

    private pure Array<float32> ToComponents(CountedChannels channels) {
        Array<float32> components = Array<float32>.Create(4, 0.0f);
        components.Set(0, channels.X);
        components.Set(1, channels.Y);
        components.Set(2, channels.Z);
        components.Set(3, channels.W);
        return components;
    }

    private pure CountedChannels FromComponents(Array<float32> components)
        => new CountedChannels { X = components.At(0), Y = components.At(1),
                                 Z = components.At(2), W = components.At(3) };

    private pure Array<float32> MutateComponents(Array<float32> components) {
        components.Set(0, components.At(0) + 10.0f);
        return components;
    }

    public pure CountedChannels RoundTrip(CountedChannels channels)
        => FromComponents(ToComponents(channels));

    public pure CountedChannels Snapshot(CountedChannels channels) {
        Array<float32> original = ToComponents(channels);
        Array<float32> modified = MutateComponents(original);
        Array<float32> alias = original;
        original.Set(1, original.At(1) + 20.0f);
        return new CountedChannels { X = original.At(0), Y = modified.At(0),
                                     Z = alias.At(1), W = original.At(1) };
    }

    private pure Array<float32> ChooseComponents(float32 value, bool select) {
        if (select)
            return Array<float32>.Create(4, value);
        return Array<float32>.Create(4, value + 1.0f);
    }

    public pure CountedChannels JoinedReturn(float32 value, bool select)
        => FromComponents(ChooseComponents(value, select));

    private pure Array<float> ToDoubles(DoubleChannels channels) {
        Array<float> components = Array<float>.Create(2, 0.0);
        components.Set(0, channels.X);
        components.Set(1, channels.Y);
        return components;
    }

    private pure DoubleChannels FromDoubles(Array<float> components)
        => new DoubleChannels { X = components.At(0), Y = components.At(1) };

    public pure DoubleChannels DoubleRoundTrip(DoubleChannels channels)
        => FromDoubles(ToDoubles(channels));

    public pure Array<float32> PublicArrayReturn(CountedChannels channels) => ToComponents(channels);
    public pure float32 PublicArrayParameter(Array<float32> components) => components.At(0);

    private pure float32 First(Array<float32> components) => components.At(0);

    public pure float32 ConflictingParameterShapes(float32 value, bool select) {
        Array<float32> first = Array<float32>.Create(3, value);
        Array<float32> second = Array<float32>.Create(4, value);
        if (select)
            return First(first);
        return First(second);
    }

    private pure Array<float32> InconsistentReturn(float32 value, bool select) {
        if (select)
            return Array<float32>.Create(3, value);
        return Array<float32>.Create(4, value);
    }

    public pure float32 ConflictingReturnShapes(float32 value, bool select)
        => InconsistentReturn(value, select).At(0);

    private pure Array<float32> DynamicComponents(float32 value, bool select) {
        int count = 4;
        if (select)
            count = 3;
        return Array<float32>.Create(count, value);
    }

    public pure float32 DynamicReturnShape(float32 value, bool select)
        => DynamicComponents(value, select).At(0);

    public pure float32 ExtraCounterWrite(float32 value) {
        int index = 0;
        while (index < 4) {
            index = index + 1;
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }

    public pure float32 MutableBound(float32 value) {
        int index = 0;
        int limit = 4;
        while (index < limit) {
            limit = limit - 1;
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }

    public pure float32 CounterNotFinal(float32 value) {
        int index = 0;
        while (index < 4) {
            index = index + 1;
            value = value + 1.0f;
        }
        return value;
    }

    public pure float32 MutableStep(float32 value) {
        int index = 0;
        int step = 1;
        while (index < 4) {
            step = step + 1;
            value = value + 1.0f;
            index = index + step;
        }
        return value;
    }

    public pure float32 NonPositiveStep(float32 value) {
        int index = 0;
        while (index < 4) {
            value = value + 1.0f;
            index = index + 0;
        }
        return value;
    }

    public pure float32 OverflowStep(float32 value) {
        int index = 9223372036854775806;
        while (index < 9223372036854775807) {
            value = value + 1.0f;
            index = index + 2;
        }
        return value;
    }

    public pure float32 InclusiveOverflow(float32 value) {
        int index = 9223372036854775807;
        while (index <= 9223372036854775807) {
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }

    public pure float32 BreakCounted(float32 value) {
        int index = 0;
        while (index < 4) {
            if (value < 0.0f)
                break;
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }

    public pure float32 ContinueCounted(float32 value) {
        int index = 0;
        while (index < 4) {
            if (value < 0.0f)
                continue;
            value = value + 1.0f;
            index = index + 1;
        }
        return value;
    }

    public pure float32 ExcessiveWork(float32 value) {
        int row = 0;
        while (row < 1000) {
            int column = 0;
            while (column < 1000) {
                value = value + 1.0f;
                column = column + 1;
            }
            row = row + 1;
        }
        return value;
    }
}
)pipe");
}

void verifyNativeNumericCountedArrayExports(const pipelang::tools::Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.CountedArrayQueries")
            methods.insert(item.at("method").get<std::string>());
    }
    require(methods == std::set<std::string>{"Nested", "RowsAfterPivot", "Stepped", "Inclusive", "ZeroTrip", "RoundTrip",
        "Snapshot", "JoinedReturn", "DoubleRoundTrip"},
        "counted-loop or private-array projection admitted unsafe shapes, work or counter mutation");
}

void verifyNativeNumericCountedArrayRejections(const std::filesystem::path& prefix,
                                              const pipelang::tools::Json& request)
{
    auto invalid = request;
    invalid["modules"][0]["source"] = invalid["modules"][0]["source"].get<std::string>() + R"pipe(
public model Class RecursiveArrayHelper {
    private pure Array<float32> Recurse(float32 value) => Recurse(value);
    public pure float32 Read(float32 value) => Recurse(value).At(0);
}
)pipe";
    const auto compiled = process({nativeExecutable(prefix / "bin/pipelang-modelc").string()},
                                  invalid.dump(), std::chrono::seconds(45), 4U << 20);
    require(compiled.exit == 0, "recursive private-array request failed to execute");
    const auto response = parseJson(compiled.output, 4U << 20);
    require(response.at("ok") == true, "bounded recursive private-array source was refused");
    for (const auto& item : response.at("manifest").at("nativeNumericExports")) {
        require(item.at("owner") != "Numeric.RecursiveArrayHelper",
                "recursive private-array closure escaped through an unguarded native export");
    }
}

std::string nativeNumericCountedArrayConsumer()
{
    return R"cpp(
template<class Query, class Channels>
concept ExposesPrivateComponents = requires { Query::ToComponents(Channels{}); };

template<class Query, class Channels, class Doubles>
bool verifyCountedArrayProjection()
{
    static_assert(!ExposesPrivateComponents<Query, Channels>);
    static_assert(noexcept(Query::Nested(0.0f)));
    static_assert(noexcept(Query::RowsAfterPivot(0.0f)));
    static_assert(noexcept(Query::Stepped(0.0f)));
    static_assert(noexcept(Query::Inclusive(0.0f)));
    static_assert(noexcept(Query::ZeroTrip(0.0f)));
    static_assert(noexcept(Query::RoundTrip(Channels{})));
    static_assert(noexcept(Query::Snapshot(Channels{})));
    static_assert(noexcept(Query::JoinedReturn(0.0f, false)));
    static_assert(noexcept(Query::DoubleRoundTrip(Doubles{})));
    for (const float value : {-2.0f, 0.0f, 1.0f, 8.0f}) {
        if (Query::Nested(value) != value * 64.0f || Query::RowsAfterPivot(value) != value * 6.0f ||
            Query::Stepped(value) != value * 2.0f || Query::Inclusive(value) != value * 4.0f ||
            Query::ZeroTrip(value) != value)
            return false;
        const Channels input{value, value + 1.0f, value + 2.0f, value + 3.0f};
        const auto snapshot = Query::Snapshot(input);
        if (snapshot.X != input.X || snapshot.Y != input.X + 10.0f ||
            snapshot.Z != input.Y || snapshot.W != input.Y + 20.0f)
            return false;
        for (const bool select : {false, true}) {
            const auto joined = Query::JoinedReturn(value, select);
            const float expected = select ? value : value + 1.0f;
            if (joined.X != expected || joined.Y != expected || joined.Z != expected || joined.W != expected)
                return false;
        }
    }
    for (const auto bits : {0u, 0x80000000u, 1u, 0x80000001u, 0x7f7fffffu, 0xff7fffffu}) {
        const float value = std::bit_cast<float>(bits);
        const Channels input{value, -value, value, -value};
        const auto output = Query::RoundTrip(input);
        if (std::bit_cast<std::uint32_t>(output.X) != bits ||
            std::bit_cast<std::uint32_t>(output.Y) != (bits ^ 0x80000000u) ||
            std::bit_cast<std::uint32_t>(output.Z) != bits ||
            std::bit_cast<std::uint32_t>(output.W) != (bits ^ 0x80000000u))
            return false;
    }
    for (const auto bits : {UINT64_C(0), UINT64_C(0x8000000000000000), UINT64_C(1),
                           UINT64_C(0x8000000000000001), UINT64_C(0x7fefffffffffffff)}) {
        const double value = std::bit_cast<double>(bits);
        const auto output = Query::DoubleRoundTrip(Doubles{value, -value});
        if (std::bit_cast<std::uint64_t>(output.X) != bits ||
            std::bit_cast<std::uint64_t>(output.Y) != (bits ^ UINT64_C(0x8000000000000000)))
            return false;
    }
    return true;
}

bool verifyNumericCountedArrays()
{
    return verifyCountedArrayProjection<numeric_fixture::native_numeric::Numeric::CountedArrayQueries,
        numeric_fixture::source::Numeric::CountedChannels, numeric_fixture::source::Numeric::DoubleChannels>() &&
        verifyCountedArrayProjection<numeric_compiled_fixture::native_numeric::Numeric::CountedArrayQueries,
            numeric_compiled_fixture::source::Numeric::CountedChannels,
            numeric_compiled_fixture::source::Numeric::DoubleChannels>();
}
)cpp";
}
