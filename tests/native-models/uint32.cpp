// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "UInt32.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using namespace uint32_models;
using pipelang::model::ExecutionFailure;
using WordResult = pipelang::model::Result<WordBox>;

namespace {
const auto maximum = std::numeric_limits<std::uint32_t>::max();

template<class Function>
void expectFailure(Function function) {
    bool refused = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        refused = true;
    }
    assert(refused);
}

std::uint32_t referenceStep(std::uint32_t state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void streamsAndSeedMix() {
    for (const auto seed : {UINT32_C(0), UINT32_C(1), UINT32_C(0x80000000), maximum,
                            UINT32_C(0x9e3779b9), UINT32_C(0x3c6ef372)}) {
        for (int rider = 0; rider < 2; ++rider) {
            const auto multiplier = static_cast<std::uint32_t>(rider + 1);
            const auto mixed = seed ^ (UINT32_C(0x9e3779b9) * multiplier);
            WordStream mixer(seed);
            assert(mixer.MixedSeed(seed, rider) == mixed);
            WordStream words(mixed);
            WordStream samples(mixed);
            WordStream unrelated(UINT32_C(0x713ac82f));
            auto reference = mixed == 0 ? UINT32_C(1) : mixed;
            for (int step = 0; step < 100000; ++step) {
                reference = referenceStep(reference);
                assert(words.Next().value() == reference);
                if (step % 3 == 0)
                    assert(unrelated.Next().is_success());
                const float expected = static_cast<float>(reference >> 8) / 16777216.0f;
                const auto actual = samples.Sample();
                assert(actual.is_success());
                assert(std::bit_cast<std::uint32_t>(actual.value()) == std::bit_cast<std::uint32_t>(expected));
                assert(words.Word() == reference && samples.Word() == reference);
            }
        }
    }
    WordStream zero(0);
    WordStream one(1);
    assert(zero.Next().value() == one.Next().value());
}

void arithmeticAndShifts() {
    WordMath model;
    assert(model.Maximum() == maximum && model.Hexadecimal() == maximum);
    assert(model.UppercaseHex() == UINT32_C(0xabcdef01));
    assert(model.Precedence() == 3);
    assert(model.UnaryPrecedence() == UINT32_C(0xfffffffc));
    assert(model.ShiftPrecedence() == 8);
    assert(model.ArithmeticPrecedence() == 6);
    for (const auto left : {UINT32_C(0), UINT32_C(1), UINT32_C(0x80000000), maximum}) {
        assert(model.BitNot(left) == static_cast<std::uint32_t>(~left));
        for (const auto right : {UINT32_C(0), UINT32_C(1), UINT32_C(0x80000000), maximum}) {
            assert(model.BitAnd(left, right) == (left & right));
            assert(model.BitOr(left, right) == (left | right));
            assert(model.BitXor(left, right) == (left ^ right));
            assert(model.Equal(left, right) == (left == right));
            assert(model.Less(left, right) == (left < right));
            assert(model.WrappingAdd(left, right) == static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) + right));
            assert(model.WrappingSubtract(left, right) == static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) - right));
            assert(model.WrappingMultiply(left, right) == static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) * right));
            for (int operation = 0; operation < 5; ++operation) {
                bool valid = true;
                std::uint64_t expected = 0;
                if (operation == 0) {
                    expected = static_cast<std::uint64_t>(left) + right;
                    valid = expected <= maximum;
                } else if (operation == 1) {
                    valid = left >= right;
                    if (valid)
                        expected = left - right;
                } else if (operation == 2) {
                    expected = static_cast<std::uint64_t>(left) * right;
                    valid = expected <= maximum;
                } else {
                    valid = right != 0;
                    if (valid && operation == 3)
                        expected = left / right;
                    if (valid && operation == 4)
                        expected = left % right;
                }
                const auto before = model.snapshot();
                const auto result = model.Calculate(left, right, operation);
                assert(result.is_success() == valid);
                if (valid) {
                    assert(result.value() == expected);
                    assert(model.snapshot().stored == left);
                } else {
                    assert(result.is_failure());
                    assert(result.error().starts_with("uint32 "));
                    assert(model.snapshot() == before);
                }
            }
        }
        for (int count = 0; count <= 31; ++count) {
            assert(model.ShiftLeft(left, count) == static_cast<std::uint32_t>(left << count));
            assert(model.ShiftRight(left, count) == static_cast<std::uint32_t>(left >> count));
        }
    }
    assert(model.WrappingAdd(maximum, 1) == 0);
    assert(model.WrappingSubtract(0, 1) == maximum);
    assert(model.WrappingMultiply(UINT32_C(0x9e3779b9), 2) == UINT32_C(0x3c6ef372));
    for (const auto count : {INT64_C(-1), INT64_C(32), std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()}) {
        expectFailure([&] { model.ShiftLeft(maximum, count); });
        expectFailure([&] { model.ShiftRight(maximum, count); });
        const auto before = model.snapshot();
        const auto failed = model.ShiftRollback(maximum, count);
        assert(failed.is_failure());
        assert(failed.error() == "uint32 shift count must be between 0 and 31");
        assert(model.snapshot() == before);
    }
}

void conversions() {
    WordMath model;
    for (const auto word : {UINT32_C(0), UINT32_C(1), UINT32_C(16777217), UINT32_C(0x80000000), maximum}) {
        assert(model.ToInt(word) == static_cast<std::int64_t>(word));
        assert(model.FromInt(static_cast<std::int64_t>(word)) == word);
        assert(model.ToDouble(word) == static_cast<double>(word));
        volatile float expected = static_cast<float>(word);
        assert(model.ToSingle(word) == expected);
    }
    for (const auto value : {INT64_C(-1), INT64_C(4294967296), std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()})
        expectFailure([&] { model.FromInt(value); });
    for (const double value : {-0.75, -0.0, 0.0, 0.75, 1.75, 4294967295.0, 4294967295.75})
        assert(model.FromDouble(value) == static_cast<std::uint32_t>(std::trunc(value)));
    for (const float value : {-0.75f, -0.0f, 0.0f, 1.75f, std::nextafter(0x1p32f, 0.0f)})
        assert(model.FromSingle(value) == static_cast<std::uint32_t>(std::trunc(value)));
    for (const double value : {-1.0, -std::numeric_limits<double>::max(), 0x1p32,
                               std::numeric_limits<double>::max(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        expectFailure([&] { model.FromDouble(value); });
        const auto before = model.snapshot();
        assert(model.ConversionRollback(value).is_failure());
        assert(model.snapshot() == before);
    }
    for (const float value : {-1.0f, 0x1p32f, std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        expectFailure([&] { model.FromSingle(value); });
}

class EchoHost final : public WordHost {
public:
    WordResult Echo(WordBox value) override {
        if (throwing)
            throw std::runtime_error("uint32 host failure");
        return WordResult::Success(value);
    }
    bool throwing = false;
};

void recordAndHostTransport() {
    auto host = std::make_shared<EchoHost>();
    WordTransport model(host);
    assert(model.Echo(maximum).value().word == maximum);
    const auto before = model.snapshot();
    host->throwing = true;
    assert(model.Echo(UINT32_C(0x80000000)).is_failure());
    assert(model.snapshot() == before);
}
}

int main() {
    arithmeticAndShifts();
    conversions();
    streamsAndSeedMix();
    recordAndHostTransport();
}
