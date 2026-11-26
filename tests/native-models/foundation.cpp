// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Foundation.constants.hpp"
#include "Foundation.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <bit>
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace pipelang::model;
using namespace foundation;

namespace {
struct Queries final : MathQueries {
    mutable int observed = 0;
    int mode = 0;
    std::function<void()> reenter;

    Result<float, std::string> Scale(float value) const override {
        ++observed;
        if (reenter)
            reenter();
        if (mode == 1)
            return Result<float, std::string>::Failure("unavailable");
        if (mode == 2)
            return Result<float, std::string>::Cancelled();
        if (mode == 3)
            throw std::runtime_error("native query fault");
        return Result<float, std::string>::Success(value * 2.0f);
    }

    Result<List<std::int64_t>, std::string> Copy(List<std::int64_t> values) const override {
        values.add(9);
        return Result<List<std::int64_t>, std::string>::Success(std::move(values));
    }

    Result<void, std::string> Touch() override {
        return Result<void, std::string>::Success();
    }
};

template<class Callback>
std::int64_t budgetBoundary(Callback callback) {
    std::int64_t lower = 0;
    std::int64_t upper = 10000;
    while (lower + 1 < upper) {
        const auto middle = lower + (upper - lower) / 2;
        try {
            assert(callback(middle) == middle * 42);
            lower = middle;
        } catch (const EvaluationLimitFailure&) {
            upper = middle;
        }
    }
    return lower;
}

void constantsAndStrings(App& model) {
    static_assert(App::Gates == 18);
    static_assert(constants::Numbers::SafeRemainder == 0);
    static_assert(constants::Numbers::HighBit == UINT32_C(0x80000000));
    static_assert(constants::Numbers::Enabled);
    static_assert(std::bit_cast<std::uint32_t>(constants::Numbers::NegativeZero) == UINT32_C(0x80000000));
    static_assert(std::bit_cast<std::uint32_t>(constants::Numbers::Low) == UINT32_C(0xff800000));
    volatile float turn = 96.0f * 3.14159265358979323846f;
    volatile float straightTwice = 624.0f * 2.0f;
    volatile float turnTwice = turn * 2.0f;
    volatile float length = straightTwice + turnTwice;
    volatile float spacing = length / 18.0f;
    assert(std::bit_cast<std::uint32_t>(model.ReadLength()) == std::bit_cast<std::uint32_t>(float(length)));
    assert(std::bit_cast<std::uint32_t>(model.ReadSpacing()) == std::bit_cast<std::uint32_t>(float(spacing)));
    assert(App::Length == model.ReadLength() && App::GateSpacing == model.ReadSpacing());
    assert(std::isinf(model.Low()) && std::signbit(model.Low()));
    assert(std::isinf(model.High()) && !std::signbit(model.High()));
    assert(std::bit_cast<std::uint32_t>(model.NotANumber()) == UINT32_C(0x7fc00000));
    assert(std::isinf(model.LowWide()) && std::signbit(model.LowWide()));
    assert(std::isinf(model.HighWide()) && !std::signbit(model.HighWide()));
    assert(std::bit_cast<std::uint64_t>(model.NotANumberWide()) == UINT64_C(0x7ff8000000000000));
    assert(model.Prefix("dlss-unknown", "dlss-"));
    assert(!model.Prefix("DLSS-quality", "dlss-"));
    assert(model.Prefix("", "") && model.Prefix("value", ""));
    assert(!model.Prefix("", "x") && !model.Prefix("dlss", "dlss-"));
    assert(model.Prefix(std::string("a\0b", 3), std::string("a\0", 2)));
    assert(!model.Prefix(std::string("a\0b", 3), std::string("a\0c", 3)));
    assert(model.Prefix("\xc3\xa9lan", "\xc3\xa9"));
}

template<class Callback>
void expectTextFailure(Callback callback) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

void utf8Text(App& model) {
    const std::string text = "a\xc3\xa9\xf0\x9f\x9a\x80z";
    assert(model.ByteLength(text) == 8);
    assert(model.ByteLength("") == 0);
    assert(model.ByteLength(std::string("a\0b", 3)) == 3);
    assert(model.ByteSlice(text, 1, 2) == "\xc3\xa9");
    assert(model.ByteSlice(text, 3, 4) == "\xf0\x9f\x9a\x80");
    assert(model.ByteSlice(text, 0, 8) == text);
    assert(model.ByteSlice(text, 8, 0).empty());
    assert(model.ByteSlice("", 0, 0).empty());
    assert(model.ByteIndexOf(text, "\xc3\xa9", 0) == 1);
    assert(model.ByteIndexOf(text, "z", 3) == 7);
    assert(model.ByteIndexOf(text, "a", 1) == -1);
    assert(model.ByteIndexOf(text, "z", 8) == -1);
    assert(model.ByteIndexOf(text, "", 8) == 8);
    assert(model.ByteIndexOf("", "", 0) == 0);
    assert(model.ByteIndexOf(std::string("a\0b", 3), std::string("\0b", 2), 0) == 1);
    for (const auto start : {-1, 2, 4, 5, 6, 9}) {
        expectTextFailure([&] { (void)model.ByteSlice(text, start, 0); });
        expectTextFailure([&] { (void)model.ByteIndexOf(text, "", start); });
    }
    expectTextFailure([&] { (void)model.ByteSlice(text, 0, -1); });
    expectTextFailure([&] { (void)model.ByteSlice(text, 0, 2); });
    expectTextFailure([&] { (void)model.ByteSlice(text, 0, INT64_MAX); });
    expectTextFailure([&] { (void)model.ByteIndexOf(text, "", INT64_MIN); });
    expectTextFailure([&] { (void)model.ByteLength(std::string("\xff", 1)); });
    expectTextFailure([&] { (void)model.ByteIndexOf(text, std::string("\x80", 1), 0); });
    expectTextFailure([&] { (void)model.ByteLength(std::string(maximumStringBytes + 1, 'x')); });
    const std::string maximum(maximumStringBytes, 'x');
    assert(model.ByteLength(maximum) == static_cast<std::int64_t>(maximumStringBytes));
    assert(model.ByteSlice(maximum, 0, maximumStringBytes) == maximum);
    assert(model.IntegerText(0) == "0");
    assert(model.IntegerText(42) == "42");
    assert(model.IntegerText(-42) == "-42");
    assert(model.IntegerText(INT64_MAX) == "9223372036854775807");
    assert(model.IntegerText(INT64_MIN) == "-9223372036854775808");
    assert(model.OrderedSlice().value() == "bc" && model.snapshot().sequence == 123);
    assert(model.OrderedSearch().value() == 1 && model.snapshot().sequence == 123);
    assert(model.OrderedInteger().value() == "-42" && model.snapshot().sequence == 1);
    const auto calls = model.snapshot().calls;
    assert(model.InvalidSlice().is_failure());
    assert(model.snapshot().calls == calls);
}
}

int main() {
    auto queries = std::make_shared<Queries>();
    App model(queries);
    constantsAndStrings(model);
    utf8Text(model);
    Numbers foldedOnly(TaskOptions{4, 1});
    (void)foldedOnly;
    const auto literalBoundary = budgetBoundary([&](std::int64_t count) { return model.SumLiteral(count); });
    const auto constantBoundary = budgetBoundary([&](std::int64_t count) { return model.SumConstant(count); });
    assert(literalBoundary > 0 && literalBoundary == constantBoundary);
    assert(model.Query(3.0f).value() == 6.0f);
    queries->mode = 1;
    assert(model.Query(3.0f).is_failure() && model.Query(3.0f).error() == "unavailable");
    queries->mode = 2;
    assert(model.Query(3.0f).is_cancelled());
    queries->mode = 3;
    bool nativeFault = false;
    try {
        (void)model.Query(3.0f);
    } catch (const ExecutionFailure& error) {
        nativeFault = true;
        assert(error.what() == std::string("native query fault"));
    }
    assert(nativeFault);
    queries->mode = 0;
    bool rejected = false;
    queries->reenter = [&] {
        try {
            (void)model.ReadLength();
        } catch (const ExecutionFailure&) {
            rejected = true;
        }
    };
    assert(model.Query(1.0f).is_success() && rejected);
    queries->reenter = {};
    List<std::int64_t> input;
    input.add(4);
    const auto copied = model.Copy(input);
    assert(copied.is_success() && copied.value().count() == 2 && input.count() == 1);
    std::thread foreign([&] {
        bool failed = false;
        try {
            (void)model.Query(2.0f);
        } catch (const ExecutionFailure&) {
            failed = true;
        }
        assert(failed);
    });
    foreign.join();
    assert(model.snapshot().calls == 0);
    assert(model.Touch().is_success() && model.snapshot().calls == 1);
}
