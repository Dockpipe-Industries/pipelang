// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pipelang::sqlite {
struct Value {
    enum class Kind { Null, Integer, Real, Text, Blob };
    Kind kind = Kind::Null;
    std::int64_t integer = 0;
    double real = 0;
    std::string text;
    std::vector<std::uint8_t> blob;

    static Value Null() {
        return {};
    }
    static Value Integer(std::int64_t input) {
        Value value;
        value.kind = Kind::Integer;
        value.integer = input;
        return value;
    }
    static Value Real(double input) {
        Value value;
        value.kind = Kind::Real;
        value.real = input;
        return value;
    }
    static Value Text(std::string input) {
        Value value;
        value.kind = Kind::Text;
        value.text = std::move(input);
        return value;
    }
    static Value Blob(std::vector<std::uint8_t> input) {
        Value value;
        value.kind = Kind::Blob;
        value.blob = std::move(input);
        return value;
    }
    bool operator==(const Value&) const = default;
};
}
