// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeByteTextFixture.hpp"

#include <set>

using namespace pipelang::tools;

void writeNativeByteTextFixture(const std::filesystem::path& output)
{
    atomicWrite(output / "src/ByteText.pipe", R"pipe(module Numeric;
public Enum ByteError { Invalid }
public model Class ByteText {
    public pure bool Length(string text, int expected) => string.ByteLengthIs(text, expected);
    public pure int Read(string text, int index, int fallback) => string.ByteAtOr(text, index, fallback);
    private pure int First(string text) => string.ByteAtOr(text, 0, -1);
    public pure int Forward(string text) => First(text);
    public pure int Qualified(string text) => ByteText.Length(text, 0) ? -1 : ByteText.Read(text, 0, -1);
    public pure int Sum(string text) {
        if (!string.ByteLengthIs(text, 4))
            return -1;
        int total = 0;
        int index = 0;
        while (index < 4) {
            total = total + string.ByteAtOr(text, index, -1);
            index = index + 1;
        }
        return total;
    }
    private pure Result<int, ByteError> Bounded(int value) {
        if (value < 0 || value > 255)
            return Result<int, ByteError>.Failure(ByteError.Invalid);
        return Result<int, ByteError>.Success(value);
    }
    public pure int Composed(string text) {
        Array<int> values = Array<int>.Create(4, 0);
        int index = 0;
        while (index < 4) {
            Result<int, ByteError> item = Bounded(string.ByteAtOr(text, index, -1));
            if (!item.IsSuccess)
                return -1;
            values.Set(index, item.Value * 257);
            index = index + 1;
        }
        return values.At(0) + values.At(1) + values.At(2) + values.At(3);
    }
    private pure int Ten(int value) {
        if (value < 0 || value > 10)
            return 0;
        return value;
    }
    public pure int Summary(int value) => Ten(value) * 3;
    public pure int Snapshot(int value) {
        Array<int> original = Array<int>.Create(2, 5);
        Array<int> alias = original;
        original.Set(0, value);
        return alias.At(0) * 2;
    }
    private pure Array<int> Replace(Array<int> values, int replacement) {
        values.Set(0, replacement);
        return values;
    }
    public pure int UnknownHelper(int replacement) {
        Array<int> original = Array<int>.Create(2, 5);
        Array<int> modified = Replace(original, replacement);
        return modified.At(0) * 2;
    }
    public pure int BranchSnapshot(int replacement, bool select) {
        Array<int> original = Array<int>.Create(2, 5);
        Array<int> alias = original;
        if (select)
            original.Set(0, replacement);
        else
            original.Set(1, replacement);
        return alias.At(0) * 2;
    }
    public pure int UnknownPayload(Result<int, ByteError> input) {
        if (!input.IsSuccess)
            return 0;
        return input.Value * 16;
    }
    public pure int UnknownArray(int value) {
        Array<int> values = Array<int>.Create(2, value);
        return values.At(0) * 2;
    }
    public pure int MutationWidens(int value) {
        Array<int> values = Array<int>.Create(2, 0);
        values.Set(1, value);
        return values.At(0) * 2;
    }
    public pure string ReturnText(string text) => text;
    public pure bool Compare(string first, string second) => first == second;
    public pure bool Prefix(string first, string second) => string.StartsWith(first, second);
    public pure int Copy(string text) {
        string copy = text;
        return string.ByteAtOr(copy, 0, -1);
    }
    public pure int Literal() => string.ByteAtOr("literal", 0, -1);
}
)pipe");
}

void verifyNativeByteTextExports(const Json& manifest)
{
    std::set<std::string> methods;
    for (const auto& item : manifest.at("nativeNumericExports")) {
        if (item.at("owner") == "Numeric.ByteText") {
            require(item.at("allocationFree") == true && item.at("noexcept") == true &&
                item.at("binary32Environment") == "not-required", "borrowed text export contract changed");
            methods.insert(item.at("method").get<std::string>());
        }
    }
    require(methods == std::set<std::string>{"Length", "Read", "Forward", "Qualified", "Sum", "Composed", "Summary", "Snapshot", "BranchSnapshot"},
        "borrowed text projection admitted ownership, unbounded work or omitted safe operations");
}

std::string nativeByteTextConsumer()
{
    return R"cpp(
template<class Query>
bool verifyBorrowedBytes()
{
    static_assert(std::is_same_v<decltype(&Query::Read),
        std::int64_t (*)(std::string_view, std::int64_t, std::int64_t) noexcept>);
    static_assert(noexcept(Query::Length({}, 0)));
    const char bytes[] = {'a', '\0', static_cast<char>(0x80), static_cast<char>(0xff)};
    const std::string_view input(bytes, sizeof(bytes));
    if (!Query::Length(input, 4) || Query::Length(input, -1) || Query::Length(input, INT64_MAX) ||
        !Query::Length({}, 0) || Query::Length(input, 0))
        return false;
    for (const std::int64_t fallback : {INT64_MIN, INT64_C(-1), INT64_C(0), INT64_C(256), INT64_MAX}) {
        for (const std::int64_t index : {INT64_MIN, INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(2),
                                       INT64_C(3), INT64_C(4), INT64_MAX}) {
            const auto expected = index >= 0 && index < 4
                ? static_cast<std::int64_t>(static_cast<unsigned char>(bytes[index])) : fallback;
            if (Query::Read(input, index, fallback) != expected || Query::Read({}, index, fallback) != fallback)
                return false;
        }
    }
    if (Query::Sum(input) != 480 || Query::Sum(input.substr(1)) != -1 || Query::Forward(input) != 97 ||
        Query::Forward({}) != -1 || Query::Qualified(input) != 97 || Query::Qualified({}) != -1)
        return false;
    if (Query::Composed(input) != 480 * 257 || Query::Composed(input.substr(1)) != -1 ||
        Query::Summary(7) != 21 || Query::Summary(INT64_MAX) != 0 || Query::Summary(INT64_MIN) != 0 ||
        Query::Snapshot(INT64_MAX) != 10 || Query::Snapshot(INT64_MIN) != 10 ||
        Query::BranchSnapshot(INT64_MAX, true) != 10 || Query::BranchSnapshot(INT64_MIN, false) != 10)
        return false;
    for (unsigned byte = 0; byte <= 255; ++byte) {
        const char item = static_cast<char>(byte);
        if (Query::Read(std::string_view(&item, 1), 0, -1) != byte)
            return false;
    }
    return true;
}

bool verifyNativeByteText()
{
    using Ordinary = numeric_fixture::source::Numeric::ByteText;
    using CompiledOrdinary = numeric_compiled_fixture::source::Numeric::ByteText;
    static_assert(std::is_same_v<decltype(&Ordinary::Read),
        std::int64_t (Ordinary::*)(std::string, std::int64_t, std::int64_t) const>);
    static_assert(std::is_same_v<decltype(&CompiledOrdinary::Read),
        std::int64_t (CompiledOrdinary::*)(std::string, std::int64_t, std::int64_t) const>);
    return verifyBorrowedBytes<numeric_fixture::native_numeric::Numeric::ByteText>() &&
        verifyBorrowedBytes<numeric_compiled_fixture::native_numeric::Numeric::ByteText>();
}
)cpp";
}
